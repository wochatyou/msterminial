// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "textBuffer.hpp"

#include <til/hash.h>
#include <til/unicode.h>

#include "UTextAdapter.h"
#include "../../types/inc/GlyphWidth.hpp"
#include "../renderer/base/renderer.hpp"
#include "../types/inc/convert.hpp"
#include "../types/inc/utils.hpp"

using namespace Microsoft::Console;
using namespace Microsoft::Console::Types;

using PointTree = interval_tree::IntervalTree<til::point, size_t>;

constexpr bool allWhitespace(const std::wstring_view& text) noexcept
{
    for (const auto ch : text)
    {
        if (ch != L' ')
        {
            return false;
        }
    }
    return true;
}

static std::atomic<uint64_t> s_lastMutationIdInitialValue;

// Routine Description:
// - Creates a new instance of TextBuffer
// Arguments:
// - screenBufferSize - The X by Y dimensions of the new screen buffer
// - defaultAttributes - The attributes with which the buffer will be initialized
// - cursorSize - The height of the cursor within this buffer
// - isActiveBuffer - Whether this is the currently active buffer
// - renderer - The renderer to use for triggering a redraw
// Return Value:
// - constructed object
// Note: may throw exception
TextBuffer::TextBuffer(til::size screenBufferSize,
                       const TextAttribute defaultAttributes,
                       const UINT cursorSize,
                       const bool isActiveBuffer,
                       Microsoft::Console::Render::Renderer& renderer) :
    _renderer{ renderer },
    _currentAttributes{ defaultAttributes },
    // This way every TextBuffer will start with a ""unique"" _lastMutationId
    // and so it'll compare unequal with the counter of other TextBuffers.
    _lastMutationId{ s_lastMutationIdInitialValue.fetch_add(0x100000000) },
    _cursor{ cursorSize, *this },
    _isActiveBuffer{ isActiveBuffer }
{
    // Guard against resizing the text buffer to 0 columns/rows, which would break being able to insert text.
    screenBufferSize.width = std::max(screenBufferSize.width, 1);
    screenBufferSize.height = std::max(screenBufferSize.height, 1);
    _reserve(screenBufferSize, defaultAttributes);
}

TextBuffer::~TextBuffer()
{
    if (_buffer)
    {
        _destroy();
    }
}

// I put these functions in a block at the start of the class, because they're the most
// fundamental aspect of TextBuffer: It implements the basic gap buffer text storage.
// It's also fairly tricky code.
#pragma region buffer management
#pragma warning(push)
#pragma warning(disable : 26481) // Don't use pointer arithmetic. Use span instead (bounds.1).
#pragma warning(disable : 26490) // Don't use reinterpret_cast (type.1).

// MEM_RESERVEs memory sufficient to store height-many ROW structs,
// as well as their ROW::_chars and ROW::_charOffsets buffers.
//
// We use explicit virtual memory allocations to not taint the general purpose allocator
// with our huge allocation, as well as to be able to reduce the private working set of
// the application by only committing what we actually need. This reduces conhost's
// memory usage from ~7MB down to just ~2MB at startup in the general case.
void TextBuffer::_reserve(til::size screenBufferSize, const TextAttribute& defaultAttributes)
{
    const auto w = gsl::narrow<uint16_t>(screenBufferSize.width);
    const auto h = gsl::narrow<uint16_t>(screenBufferSize.height);

    constexpr auto rowSize = ROW::CalculateRowSize();
    const auto charsBufferSize = ROW::CalculateCharsBufferSize(w);
    const auto charOffsetsBufferSize = ROW::CalculateCharOffsetsBufferSize(w);
    const auto rowStride = rowSize + charsBufferSize + charOffsetsBufferSize;
    assert(rowStride % alignof(ROW) == 0);

    // 65535*65535 cells would result in a allocSize of 8GiB.
    // --> Use uint64_t so that we can safely do our calculations even on x86.
    // We allocate 1 additional row, which will be used for GetScratchpadRow().
    const auto rowCount = ::base::strict_cast<uint64_t>(h) + 1;
    const auto allocSize = gsl::narrow<size_t>(rowCount * rowStride);

    // NOTE: Modifications to this block of code might have to be mirrored over to ResizeTraditional().
    // It constructs a temporary TextBuffer and then extracts the members below, overwriting itself.
    _buffer = wil::unique_virtualalloc_ptr<std::byte>{
        static_cast<std::byte*>(THROW_LAST_ERROR_IF_NULL(VirtualAlloc(nullptr, allocSize, MEM_RESERVE, PAGE_READWRITE)))
    };
    _bufferEnd = _buffer.get() + allocSize;
    _commitWatermark = _buffer.get();
    _initialAttributes = defaultAttributes;
    _bufferRowStride = rowStride;
    _bufferOffsetChars = rowSize;
    _bufferOffsetCharOffsets = rowSize + charsBufferSize;
    _width = w;
    _height = h;
}

// MEM_COMMITs the memory and constructs all ROWs up to and including the given row pointer.
// It's expected that the caller verifies the parameter. It goes hand in hand with _getRowByOffsetDirect().
//
// Declaring this function as noinline allows _getRowByOffsetDirect() to be inlined,
// which improves overall TextBuffer performance by ~6%. And all it cost is this annotation.
// The compiler doesn't understand the likelihood of our branches. (PGO does, but that's imperfect.)
__declspec(noinline) void TextBuffer::_commit(const std::byte* row)
{
    const auto rowEnd = row + _bufferRowStride;
    const auto remaining = gsl::narrow_cast<uintptr_t>(_bufferEnd - _commitWatermark);
    const auto minimum = gsl::narrow_cast<uintptr_t>(rowEnd - _commitWatermark);
    const auto ideal = minimum + _bufferRowStride * _commitReadAheadRowCount;
    const auto size = std::min(remaining, ideal);

    THROW_LAST_ERROR_IF_NULL(VirtualAlloc(_commitWatermark, size, MEM_COMMIT, PAGE_READWRITE));

    _construct(_commitWatermark + size);
}

// Destructs and MEM_DECOMMITs all previously constructed ROWs.
// You can use this (or rather the Reset() method) to fully clear the TextBuffer.
void TextBuffer::_decommit() noexcept
{
    _destroy();
    VirtualFree(_buffer.get(), 0, MEM_DECOMMIT);
    _commitWatermark = _buffer.get();
}

// Constructs ROWs up to (excluding) the ROW pointed to by `until`.
void TextBuffer::_construct(const std::byte* until) noexcept
{
    for (; _commitWatermark < until; _commitWatermark += _bufferRowStride)
    {
        const auto row = reinterpret_cast<ROW*>(_commitWatermark);
        const auto chars = reinterpret_cast<wchar_t*>(_commitWatermark + _bufferOffsetChars);
        const auto indices = reinterpret_cast<uint16_t*>(_commitWatermark + _bufferOffsetCharOffsets);
        std::construct_at(row, chars, indices, _width, _initialAttributes);
    }
}

// Destroys all previously constructed ROWs.
// Be careful! This doesn't reset any of the members, in particular the _commitWatermark.
void TextBuffer::_destroy() const noexcept
{
    for (auto it = _buffer.get(); it < _commitWatermark; it += _bufferRowStride)
    {
        std::destroy_at(reinterpret_cast<ROW*>(it));
    }
}

// This function is "direct" because it trusts the caller to properly wrap the "offset"
// parameter modulo the _height of the buffer, etc. But keep in mind that a offset=0
// is the GetScratchpadRow() and not the GetRowByOffset(0). That one is offset=1.
ROW& TextBuffer::_getRowByOffsetDirect(size_t offset)
{
    const auto row = _buffer.get() + _bufferRowStride * offset;
    THROW_HR_IF(E_UNEXPECTED, row < _buffer.get() || row >= _bufferEnd);

    if (row >= _commitWatermark)
    {
        _commit(row);
    }

    return *reinterpret_cast<ROW*>(row);
}

ROW& TextBuffer::_getRow(til::CoordType y) const
{
    // Rows are stored circularly, so the index you ask for is offset by the start position and mod the total of rows.
    auto offset = (_firstRow + y) % _height;

    // Support negative wrap around. This way an index of -1 will
    // wrap to _rowCount-1 and make implementing scrolling easier.
    if (offset < 0)
    {
        offset += _height;
    }

    // We add 1 to the row offset, because row "0" is the one returned by GetScratchpadRow().
#pragma warning(suppress : 26492) // Don't use const_cast to cast away const or volatile (type.3).
    return const_cast<TextBuffer*>(this)->_getRowByOffsetDirect(gsl::narrow_cast<size_t>(offset) + 1);
}

// Returns the "user-visible" index of the last committed row, which can be used
// to short-circuit some algorithms that try to scan the entire buffer.
// Returns 0 if no rows are committed in.
til::CoordType TextBuffer::_estimateOffsetOfLastCommittedRow() const noexcept
{
    const auto lastRowOffset = (_commitWatermark - _buffer.get()) / _bufferRowStride;
    // This subtracts 2 from the offset to account for the:
    // * scratchpad row at offset 0, whereas regular rows start at offset 1.
    // * fact that _commitWatermark points _past_ the last committed row,
    //   but we want to return an index pointing at the last row.
    return std::max(0, gsl::narrow_cast<til::CoordType>(lastRowOffset - 2));
}

// Retrieves a row from the buffer by its offset from the first row of the text buffer
// (what corresponds to the top row of the screen buffer).
const ROW& TextBuffer::GetRowByOffset(const til::CoordType index) const
{
    return _getRow(index);
}

// Retrieves a row from the buffer by its offset from the first row of the text buffer
// (what corresponds to the top row of the screen buffer).
ROW& TextBuffer::GetMutableRowByOffset(const til::CoordType index)
{
    _lastMutationId++;
    return _getRow(index);
}

// Returns a row filled with whitespace and the current attributes, for you to freely use.
ROW& TextBuffer::GetScratchpadRow()
{
    return GetScratchpadRow(_currentAttributes);
}

// Returns a row filled with whitespace and the given attributes, for you to freely use.
ROW& TextBuffer::GetScratchpadRow(const TextAttribute& attributes)
{
    auto& r = _getRowByOffsetDirect(0);
    r.Reset(attributes);
    return r;
}

#pragma warning(pop)
#pragma endregion

// Routine Description:
// - Copies properties from another text buffer into this one.
// - This is primarily to copy properties that would otherwise not be specified during CreateInstance
// Arguments:
// - OtherBuffer - The text buffer to copy properties from
// Return Value:
// - <none>
void TextBuffer::CopyProperties(const TextBuffer& OtherBuffer) noexcept
{
    GetCursor().CopyProperties(OtherBuffer.GetCursor());
}

// Routine Description:
// - Gets the number of rows in the buffer
// Arguments:
// - <none>
// Return Value:
// - Total number of rows in the buffer
til::CoordType TextBuffer::TotalRowCount() const noexcept
{
    return _height;
}

// Method Description:
// - Gets the number of glyphs in the buffer between two points.
// - IMPORTANT: Make sure that start is before end, or this will never return!
// Arguments:
// - start - The starting point of the range to get the glyph count for.
// - end - The ending point of the range to get the glyph count for.
// Return Value:
// - The number of glyphs in the buffer between the two points.
size_t TextBuffer::GetCellDistance(const til::point from, const til::point to) const
{
    auto startCell = GetCellDataAt(from);
    const auto endCell = GetCellDataAt(to);
    auto delta = 0;
    while (startCell != endCell)
    {
        ++startCell;
        ++delta;
    }
    return delta;
}

// Routine Description:
// - Retrieves read-only text iterator at the given buffer location
// Arguments:
// - at - X,Y position in buffer for iterator start position
// Return Value:
// - Read-only iterator of text data only.
TextBufferTextIterator TextBuffer::GetTextDataAt(const til::point at) const
{
    return TextBufferTextIterator(GetCellDataAt(at));
}

// Routine Description:
// - Retrieves read-only cell iterator at the given buffer location
// Arguments:
// - at - X,Y position in buffer for iterator start position
// Return Value:
// - Read-only iterator of cell data.
TextBufferCellIterator TextBuffer::GetCellDataAt(const til::point at) const
{
    return TextBufferCellIterator(*this, at);
}

// Routine Description:
// - Retrieves read-only text iterator at the given buffer location
//   but restricted to only the specific line (Y coordinate).
// Arguments:
// - at - X,Y position in buffer for iterator start position
// Return Value:
// - Read-only iterator of text data only.
TextBufferTextIterator TextBuffer::GetTextLineDataAt(const til::point at) const
{
    return TextBufferTextIterator(GetCellLineDataAt(at));
}

// Routine Description:
// - Retrieves read-only cell iterator at the given buffer location
//   but restricted to only the specific line (Y coordinate).
// Arguments:
// - at - X,Y position in buffer for iterator start position
// Return Value:
// - Read-only iterator of cell data.
TextBufferCellIterator TextBuffer::GetCellLineDataAt(const til::point at) const
{
    til::inclusive_rect limit;
    limit.top = at.y;
    limit.bottom = at.y;
    limit.left = 0;
    limit.right = GetSize().RightInclusive();

    return TextBufferCellIterator(*this, at, Viewport::FromInclusive(limit));
}

// Routine Description:
// - Retrieves read-only text iterator at the given buffer location
//   but restricted to operate only inside the given viewport.
// Arguments:
// - at - X,Y position in buffer for iterator start position
// - limit - boundaries for the iterator to operate within
// Return Value:
// - Read-only iterator of text data only.
TextBufferTextIterator TextBuffer::GetTextDataAt(const til::point at, const Viewport limit) const
{
    return TextBufferTextIterator(GetCellDataAt(at, limit));
}

// Routine Description:
// - Retrieves read-only cell iterator at the given buffer location
//   but restricted to operate only inside the given viewport.
// Arguments:
// - at - X,Y position in buffer for iterator start position
// - limit - boundaries for the iterator to operate within
// Return Value:
// - Read-only iterator of cell data.
TextBufferCellIterator TextBuffer::GetCellDataAt(const til::point at, const Viewport limit) const
{
    return TextBufferCellIterator(*this, at, limit);
}

//Routine Description:
// - Call before inserting a character into the buffer.
// - This will ensure a consistent double byte state (KAttrs line) within the text buffer
// - It will attempt to correct the buffer if we're inserting an unexpected double byte character type
//   and it will pad out the buffer if we're going to split a double byte sequence across two rows.
//Arguments:
// - dbcsAttribute - Double byte information associated with the character about to be inserted into the buffer
//Return Value:
// - true if we successfully prepared the buffer and moved the cursor
// - false otherwise (out of memory)
void TextBuffer::_PrepareForDoubleByteSequence(const DbcsAttribute dbcsAttribute)
{
    // Now compensate if we don't have enough space for the upcoming double byte sequence
    // We only need to compensate for leading bytes
    if (dbcsAttribute == DbcsAttribute::Leading)
    {
        const auto cursorPosition = GetCursor().GetPosition();
        const auto lineWidth = GetLineWidth(cursorPosition.y);

        // If we're about to lead on the last column in the row, we need to add a padding space
        if (cursorPosition.x == lineWidth - 1)
        {
            // set that we're wrapping for double byte reasons
            auto& row = GetMutableRowByOffset(cursorPosition.y);
            row.SetDoubleBytePadded(true);

            // then move the cursor forward and onto the next row
            IncrementCursor();
        }
    }
}

// Given the character offset `position` in the `chars` string, this function returns the starting position of the next grapheme.
// For instance, given a `chars` of L"x\uD83D\uDE42y" and a `position` of 1 it'll return 3.
// GraphemePrev would do the exact inverse of this operation.
// In the future, these functions are expected to also deliver information about how many columns a grapheme occupies.
// (I know that mere UTF-16 code point iteration doesn't handle graphemes, but that's what we're working towards.)
size_t TextBuffer::GraphemeNext(const std::wstring_view& chars, size_t position) noexcept
{
    return til::utf16_iterate_next(chars, position);
}

// It's the counterpart to GraphemeNext. See GraphemeNext.
size_t TextBuffer::GraphemePrev(const std::wstring_view& chars, size_t position) noexcept
{
    return til::utf16_iterate_prev(chars, position);
}

// Ever wondered how much space a piece of text needs before inserting it? This function will tell you!
// It fundamentally behaves identical to the various ROW functions around `RowWriteState`.
//
// Set `columnLimit` to the amount of space that's available (e.g. `buffer_width - cursor_position.x`)
// and it'll return the amount of characters that fit into this space. The out parameter `columns`
// will contain the amount of columns this piece of text has actually used.
//
// Just like with `RowWriteState` one special case is when not all text fits into the given space:
// In that case, this function always returns exactly `columnLimit`. This distinction is important when "inserting"
// a wide glyph but there's only 1 column left. That 1 remaining column is supposed to be padded with whitespace.
size_t TextBuffer::FitTextIntoColumns(const std::wstring_view& chars, til::CoordType columnLimit, til::CoordType& columns) noexcept
{
    columnLimit = std::max(0, columnLimit);

    const auto beg = chars.begin();
    const auto end = chars.end();
    auto it = beg;
    const auto asciiEnd = beg + std::min(chars.size(), gsl::narrow_cast<size_t>(columnLimit));

    // ASCII fast-path: 1 char always corresponds to 1 column.
    for (; it != asciiEnd && *it < 0x80; ++it)
    {
    }

    const auto dist = gsl::narrow_cast<size_t>(it - beg);
    auto col = gsl::narrow_cast<til::CoordType>(dist);

    if (it == asciiEnd) [[likely]]
    {
        columns = col;
        return dist;
    }

    // Unicode slow-path where we need to count text and columns separately.
    for (;;)
    {
        auto ptr = &*it;
        const auto wch = *ptr;
        size_t len = 1;

        col++;

        // Even in our slow-path we can avoid calling IsGlyphFullWidth if the current character is ASCII.
        // It also allows us to skip the surrogate pair decoding at the same time.
        if (wch >= 0x80)
        {
            if (til::is_surrogate(wch))
            {
                const auto it2 = it + 1;
                if (til::is_leading_surrogate(wch) && it2 != end && til::is_trailing_surrogate(*it2))
                {
                    len = 2;
                }
                else
                {
                    ptr = &UNICODE_REPLACEMENT;
                }
            }

            col += IsGlyphFullWidth({ ptr, len });
        }

        // If we ran out of columns, we need to always return `columnLimit` and not `cols`,
        // because if we tried inserting a wide glyph into just 1 remaining column it will
        // fail to fit, but that remaining column still has been used up. When the caller sees
        // `columns == columnLimit` they will line-wrap and continue inserting into the next row.
        if (col > columnLimit)
        {
            columns = columnLimit;
            return gsl::narrow_cast<size_t>(it - beg);
        }

        // But if we simply ran out of text we just need to return the actual number of columns.
        it += len;
        if (it == end)
        {
            columns = col;
            return chars.size();
        }
    }
}

// Pretend as if `position` is a regular cursor in the TextBuffer.
// This function will then pretend as if you pressed the left/right arrow
// keys `distance` amount of times (negative = left, positive = right).
til::point TextBuffer::NavigateCursor(til::point position, til::CoordType distance) const
{
    const til::CoordType maxX = _width - 1;
    const til::CoordType maxY = _height - 1;
    auto x = std::clamp(position.x, 0, maxX);
    auto y = std::clamp(position.y, 0, maxY);
    auto row = &GetRowByOffset(y);

    if (distance < 0)
    {
        do
        {
            if (x > 0)
            {
                x = row->NavigateToPrevious(x);
            }
            else if (y <= 0)
            {
                break;
            }
            else
            {
                --y;
                row = &GetRowByOffset(y);
                x = row->GetReadableColumnCount() - 1;
            }
        } while (++distance != 0);
    }
    else if (distance > 0)
    {
        auto rowWidth = row->GetReadableColumnCount();

        do
        {
            if (x < rowWidth)
            {
                x = row->NavigateToNext(x);
            }
            else if (y >= maxY)
            {
                break;
            }
            else
            {
                ++y;
                row = &GetRowByOffset(y);
                rowWidth = row->GetReadableColumnCount();
                x = 0;
            }
        } while (--distance != 0);
    }

    return { x, y };
}

// This function is intended for writing regular "lines" of text as it'll set the wrap flag on the given row.
// You can continue calling the function on the same row as long as state.columnEnd < state.columnLimit.
void TextBuffer::Write(til::CoordType row, const TextAttribute& attributes, RowWriteState& state)
{
    auto& r = GetMutableRowByOffset(row);
    r.ReplaceText(state);
    r.ReplaceAttributes(state.columnBegin, state.columnEnd, attributes);
    TriggerRedraw(Viewport::FromExclusive({ state.columnBeginDirty, row, state.columnEndDirty, row + 1 }));
}

// Fills an area of the buffer with a given fill character(s) and attributes.
void TextBuffer::FillRect(const til::rect& rect, const std::wstring_view& fill, const TextAttribute& attributes)
{
    if (!rect || fill.empty())
    {
        return;
    }

    auto& scratchpad = GetScratchpadRow(attributes);

    // The scratchpad row gets reset to whitespace by default, so there's no need to
    // initialize it again. Filling with whitespace is the most common operation by far.
    if (fill != L" ")
    {
        RowWriteState state{
            .columnLimit = rect.right,
            .columnEnd = rect.left,
        };

        // Fill the scratchpad row with consecutive copies of "fill" up to the amount we need.
        //
        // We don't just create a single string with N copies of "fill" and write that at once,
        // because that might join neighboring combining marks unintentionally.
        //
        // Building the buffer this way is very wasteful and slow, but it's still 3x
        // faster than what we had before and no one complained about that either.
        // It's seldom used code and probably not worth optimizing for.
        while (state.columnEnd < rect.right)
        {
            state.columnBegin = state.columnEnd;
            state.text = fill;
            scratchpad.ReplaceText(state);
        }
    }

    // Fill the given rows with copies of the scratchpad row. That's a little
    // slower when filling just a single row, but will be much faster for >1 rows.
    {
        RowCopyTextFromState state{
            .source = scratchpad,
            .columnBegin = rect.left,
            .columnLimit = rect.right,
            .sourceColumnBegin = rect.left,
        };

        for (auto y = rect.top; y < rect.bottom; ++y)
        {
            auto& r = GetMutableRowByOffset(y);
            r.CopyTextFrom(state);
            r.ReplaceAttributes(rect.left, rect.right, attributes);
            TriggerRedraw(Viewport::FromExclusive({ state.columnBeginDirty, y, state.columnEndDirty, y + 1 }));
        }
    }
}

// Routine Description:
// - Writes cells to the output buffer. Writes at the cursor.
// Arguments:
// - givenIt - Iterator representing output cell data to write
// Return Value:
// - The final position of the iterator
OutputCellIterator TextBuffer::Write(const OutputCellIterator givenIt)
{
    const auto& cursor = GetCursor();
    const auto target = cursor.GetPosition();

    const auto finalIt = Write(givenIt, target);

    return finalIt;
}

// Routine Description:
// - Writes cells to the output buffer.
// Arguments:
// - givenIt - Iterator representing output cell data to write
// - target - the row/column to start writing the text to
// - wrap - change the wrap flag if we hit the end of the row while writing and there's still more data
// Return Value:
// - The final position of the iterator
OutputCellIterator TextBuffer::Write(const OutputCellIterator givenIt,
                                     const til::point target,
                                     const std::optional<bool> wrap)
{
    // Make mutable copy so we can walk.
    auto it = givenIt;

    // Make mutable target so we can walk down lines.
    auto lineTarget = target;

    // Get size of the text buffer so we can stay in bounds.
    const auto size = GetSize();

    // While there's still data in the iterator and we're still targeting in bounds...
    while (it && size.IsInBounds(lineTarget))
    {
        // Attempt to write as much data as possible onto this line.
        // NOTE: if wrap = true/false, we want to set the line's wrap to true/false (respectively) if we reach the end of the line
        it = WriteLine(it, lineTarget, wrap);

        // Move to the next line down.
        lineTarget.x = 0;
        ++lineTarget.y;
    }

    return it;
}

// Routine Description:
// - Writes one line of text to the output buffer.
// Arguments:
// - givenIt - The iterator that will dereference into cell data to insert
// - target - Coordinate targeted within output buffer
// - wrap - change the wrap flag if we hit the end of the row while writing and there's still more data in the iterator.
// - limitRight - Optionally restrict the right boundary for writing (e.g. stop writing earlier than the end of line)
// Return Value:
// - The iterator, but advanced to where we stopped writing. Use to find input consumed length or cells written length.
OutputCellIterator TextBuffer::WriteLine(const OutputCellIterator givenIt,
                                         const til::point target,
                                         const std::optional<bool> wrap,
                                         std::optional<til::CoordType> limitRight)
{
    // If we're not in bounds, exit early.
    if (!GetSize().IsInBounds(target))
    {
        return givenIt;
    }

    //  Get the row and write the cells
    auto& row = GetMutableRowByOffset(target.y);
    const auto newIt = row.WriteCells(givenIt, target.x, wrap, limitRight);

    // Take the cell distance written and notify that it needs to be repainted.
    const auto written = newIt.GetCellDistance(givenIt);
    const auto paint = Viewport::FromDimensions(target, { written, 1 });
    TriggerRedraw(paint);

    return newIt;
}

//Routine Description:
// - Inserts one codepoint into the buffer at the current cursor position and advances the cursor as appropriate.
//Arguments:
// - chars - The codepoint to insert
// - dbcsAttribute - Double byte information associated with the codepoint
// - bAttr - Color data associated with the character
//Return Value:
// - true if we successfully inserted the character
// - false otherwise (out of memory)
void TextBuffer::InsertCharacter(const std::wstring_view chars,
                                 const DbcsAttribute dbcsAttribute,
                                 const TextAttribute attr)
{
    // Ensure consistent buffer state for double byte characters based on the character type we're about to insert
    _PrepareForDoubleByteSequence(dbcsAttribute);

    // Get the current cursor position
    const auto iRow = GetCursor().GetPosition().y; // row stored as logical position, not array position
    const auto iCol = GetCursor().GetPosition().x; // column logical and array positions are equal.

    // Get the row associated with the given logical position
    auto& Row = GetMutableRowByOffset(iRow);

    // Store character and double byte data
    switch (dbcsAttribute)
    {
    case DbcsAttribute::Leading:
        Row.ReplaceCharacters(iCol, 2, chars);
        break;
    case DbcsAttribute::Trailing:
        Row.ReplaceCharacters(iCol - 1, 2, chars);
        break;
    default:
        Row.ReplaceCharacters(iCol, 1, chars);
        break;
    }

    // Store color data
    Row.SetAttrToEnd(iCol, attr);
    IncrementCursor();
}

//Routine Description:
// - Inserts one ucs2 codepoint into the buffer at the current cursor position and advances the cursor as appropriate.
//Arguments:
// - wch - The codepoint to insert
// - dbcsAttribute - Double byte information associated with the codepoint
// - bAttr - Color data associated with the character
//Return Value:
// - true if we successfully inserted the character
// - false otherwise (out of memory)
void TextBuffer::InsertCharacter(const wchar_t wch, const DbcsAttribute dbcsAttribute, const TextAttribute attr)
{
    InsertCharacter({ &wch, 1 }, dbcsAttribute, attr);
}

//Routine Description:
// - Finds the current row in the buffer (as indicated by the cursor position)
//   and specifies that we have forced a line wrap on that row
//Arguments:
// - <none> - Always sets to wrap
//Return Value:
// - <none>
void TextBuffer::_SetWrapOnCurrentRow()
{
    _AdjustWrapOnCurrentRow(true);
}

//Routine Description:
// - Finds the current row in the buffer (as indicated by the cursor position)
//   and specifies whether or not it should have a line wrap flag.
//Arguments:
// - fSet - True if this row has a wrap. False otherwise.
//Return Value:
// - <none>
void TextBuffer::_AdjustWrapOnCurrentRow(const bool fSet)
{
    // The vertical position of the cursor represents the current row we're manipulating.
    const auto uiCurrentRowOffset = GetCursor().GetPosition().y;

    // Set the wrap status as appropriate
    GetMutableRowByOffset(uiCurrentRowOffset).SetWrapForced(fSet);
}

//Routine Description:
// - Increments the cursor one position in the buffer as if text is being typed into the buffer.
// - NOTE: Will introduce a wrap marker if we run off the end of the current row
//Arguments:
// - <none>
//Return Value:
// - true if we successfully moved the cursor.
// - false otherwise (out of memory)
void TextBuffer::IncrementCursor()
{
    // Cursor position is stored as logical array indices (starts at 0) for the window
    // Buffer Size is specified as the "length" of the array. It would say 80 for valid values of 0-79.
    // So subtract 1 from buffer size in each direction to find the index of the final column in the buffer
    const auto iFinalColumnIndex = GetLineWidth(GetCursor().GetPosition().y) - 1;

    // Move the cursor one position to the right
    GetCursor().IncrementXPosition(1);

    // If we've passed the final valid column...
    if (GetCursor().GetPosition().x > iFinalColumnIndex)
    {
        // Then mark that we've been forced to wrap
        _SetWrapOnCurrentRow();

        // Then move the cursor to a new line
        NewlineCursor();
    }
}

//Routine Description:
// - Increments the cursor one line down in the buffer and to the beginning of the line
//Arguments:
// - <none>
//Return Value:
// - true if we successfully moved the cursor.
void TextBuffer::NewlineCursor()
{
    const auto iFinalRowIndex = GetSize().BottomInclusive();

    // Reset the cursor position to 0 and move down one line
    GetCursor().SetXPosition(0);
    GetCursor().IncrementYPosition(1);

    // If we've passed the final valid row...
    if (GetCursor().GetPosition().y > iFinalRowIndex)
    {
        // Stay on the final logical/offset row of the buffer.
        GetCursor().SetYPosition(iFinalRowIndex);

        // Instead increment the circular buffer to move us into the "oldest" row of the backing buffer
        IncrementCircularBuffer();
    }
}

//Routine Description:
// - Increments the circular buffer by one. Circular buffer is represented by FirstRow variable.
//Arguments:
// - fillAttributes - the attributes with which the recycled row will be initialized.
//Return Value:
// - true if we successfully incremented the buffer.
void TextBuffer::IncrementCircularBuffer(const TextAttribute& fillAttributes)
{
    // FirstRow is at any given point in time the array index in the circular buffer that corresponds
    // to the logical position 0 in the window (cursor coordinates and all other coordinates).
    if (_isActiveBuffer)
    {
        _renderer.TriggerFlush(true);
    }

    // Prune hyperlinks to delete obsolete references
    _PruneHyperlinks();

    // Second, clean out the old "first row" as it will become the "last row" of the buffer after the circle is performed.
    GetMutableRowByOffset(0).Reset(fillAttributes);
    {
        // Now proceed to increment.
        // Incrementing it will cause the next line down to become the new "top" of the window (the new "0" in logical coordinates)
        _firstRow++;

        // If we pass up the height of the buffer, loop back to 0.
        if (_firstRow >= GetSize().Height())
        {
            _firstRow = 0;
        }
    }
}

//Routine Description:
// - Retrieves the position of the last non-space character in the given
//   viewport
// - By default, we search the entire buffer to find the last non-space
//   character.
// - If we know the last character is within the given viewport (so we don't
//   need to check the entire buffer), we can provide a value in viewOptional
//   that we'll use to search for the last character in.
//Arguments:
// - The viewport
//Return value:
// - Coordinate position (relative to the text buffer)
til::point TextBuffer::GetLastNonSpaceCharacter(const Viewport* viewOptional) const
{
    const auto viewport = viewOptional ? *viewOptional : GetSize();

    til::point coordEndOfText;
    // Search the given viewport by starting at the bottom.
    coordEndOfText.y = std::min(viewport.BottomInclusive(), _estimateOffsetOfLastCommittedRow());

    const auto& currRow = GetRowByOffset(coordEndOfText.y);
    // The X position of the end of the valid text is the Right draw boundary (which is one beyond the final valid character)
    coordEndOfText.x = currRow.MeasureRight() - 1;

    // If the X coordinate turns out to be -1, the row was empty, we need to search backwards for the real end of text.
    const auto viewportTop = viewport.Top();
    auto fDoBackUp = (coordEndOfText.x < 0 && coordEndOfText.y > viewportTop); // this row is empty, and we're not at the top
    while (fDoBackUp)
    {
        coordEndOfText.y--;
        const auto& backupRow = GetRowByOffset(coordEndOfText.y);
        // We need to back up to the previous row if this line is empty, AND there are more rows

        coordEndOfText.x = backupRow.MeasureRight() - 1;
        fDoBackUp = (coordEndOfText.x < 0 && coordEndOfText.y > viewportTop);
    }

    // don't allow negative results
    coordEndOfText.y = std::max(coordEndOfText.y, 0);
    coordEndOfText.x = std::max(coordEndOfText.x, 0);

    return coordEndOfText;
}

// Routine Description:
// - Retrieves the position of the previous character relative to the current cursor position
// Arguments:
// - <none>
// Return Value:
// - Coordinate position in screen coordinates of the character just before the cursor.
// - NOTE: Will return 0,0 if already in the top left corner
til::point TextBuffer::_GetPreviousFromCursor() const
{
    auto coordPosition = GetCursor().GetPosition();

    // If we're not at the left edge, simply move the cursor to the left by one
    if (coordPosition.x > 0)
    {
        coordPosition.x--;
    }
    else
    {
        // Otherwise, only if we're not on the top row (e.g. we don't move anywhere in the top left corner. there is no previous)
        if (coordPosition.y > 0)
        {
            // move the cursor up one line
            coordPosition.y--;

            // and to the right edge
            coordPosition.x = GetLineWidth(coordPosition.y) - 1;
        }
    }

    return coordPosition;
}

const til::CoordType TextBuffer::GetFirstRowIndex() const noexcept
{
    return _firstRow;
}

const Viewport TextBuffer::GetSize() const noexcept
{
    return Viewport::FromDimensions({ _width, _height });
}

void TextBuffer::_SetFirstRowIndex(const til::CoordType FirstRowIndex) noexcept
{
    _firstRow = FirstRowIndex;
}

void TextBuffer::ScrollRows(const til::CoordType firstRow, til::CoordType size, const til::CoordType delta)
{
    if (delta == 0)
    {
        return;
    }

    // Since the for() loop uses !=, we must ensure that size is positive.
    // A negative size doesn't make any sense anyways.
    size = std::max(0, size);

    til::CoordType y = 0;
    til::CoordType end = 0;
    til::CoordType step = 0;

    if (delta < 0)
    {
        // The layout is like this:
        // delta is -2, size is 3, firstRow is 5
        // We want 3 rows from 5 (5, 6, and 7) to move up 2 spots.
        // --- (storage) ----
        // | 0 begin
        // | 1
        // | 2
        // | 3 A. firstRow + delta (because delta is negative)
        // | 4
        // | 5 B. firstRow
        // | 6
        // | 7
        // | 8 C. firstRow + size
        // | 9
        // | 10
        // | 11
        // - end
        // We want B to slide up to A (the negative delta) and everything from [B,C) to slide up with it.
        y = firstRow;
        end = firstRow + size;
        step = 1;
    }
    else
    {
        // The layout is like this:
        // delta is 2, size is 3, firstRow is 5
        // We want 3 rows from 5 (5, 6, and 7) to move down 2 spots.
        // --- (storage) ----
        // | 0 begin
        // | 1
        // | 2
        // | 3
        // | 4
        // | 5 A. firstRow
        // | 6
        // | 7
        // | 8 B. firstRow + size
        // | 9
        // | 10 C. firstRow + size + delta
        // | 11
        // - end
        // We want B-1 to slide down to C-1 (the positive delta) and everything from [A, B) to slide down with it.
        y = firstRow + size - 1;
        end = firstRow - 1;
        step = -1;
    }

    for (; y != end; y += step)
    {
        GetMutableRowByOffset(y + delta).CopyFrom(GetRowByOffset(y));
    }
}

Cursor& TextBuffer::GetCursor() noexcept
{
    return _cursor;
}

const Cursor& TextBuffer::GetCursor() const noexcept
{
    return _cursor;
}

uint64_t TextBuffer::GetLastMutationId() const noexcept
{
    return _lastMutationId;
}

const TextAttribute& TextBuffer::GetCurrentAttributes() const noexcept
{
    return _currentAttributes;
}

void TextBuffer::SetCurrentAttributes(const TextAttribute& currentAttributes) noexcept
{
    _currentAttributes = currentAttributes;
}

void TextBuffer::SetWrapForced(const til::CoordType y, bool wrap)
{
    GetMutableRowByOffset(y).SetWrapForced(wrap);
}

void TextBuffer::SetCurrentLineRendition(const LineRendition lineRendition, const TextAttribute& fillAttributes)
{
    const auto cursorPosition = GetCursor().GetPosition();
    const auto rowIndex = cursorPosition.y;
    auto& row = GetMutableRowByOffset(rowIndex);
    if (row.GetLineRendition() != lineRendition)
    {
        row.SetLineRendition(lineRendition);
        // If the line rendition has changed, the row can no longer be wrapped.
        row.SetWrapForced(false);
        // And if it's no longer single width, the right half of the row should be erased.
        if (lineRendition != LineRendition::SingleWidth)
        {
            const auto fillChar = L' ';
            const auto fillOffset = GetLineWidth(rowIndex);
            const auto fillLength = gsl::narrow<size_t>(GetSize().Width() - fillOffset);
            const OutputCellIterator fillData{ fillChar, fillAttributes, fillLength };
            row.WriteCells(fillData, fillOffset, false);
            // We also need to make sure the cursor is clamped within the new width.
            GetCursor().SetPosition(ClampPositionWithinLine(cursorPosition));
        }
        TriggerRedraw(Viewport::FromDimensions({ 0, rowIndex }, { GetSize().Width(), 1 }));
    }
}

void TextBuffer::ResetLineRenditionRange(const til::CoordType startRow, const til::CoordType endRow)
{
    for (auto row = startRow; row < endRow; row++)
    {
        GetMutableRowByOffset(row).SetLineRendition(LineRendition::SingleWidth);
    }
}

LineRendition TextBuffer::GetLineRendition(const til::CoordType row) const
{
    return GetRowByOffset(row).GetLineRendition();
}

bool TextBuffer::IsDoubleWidthLine(const til::CoordType row) const
{
    return GetLineRendition(row) != LineRendition::SingleWidth;
}

til::CoordType TextBuffer::GetLineWidth(const til::CoordType row) const
{
    // Use shift right to quickly divide the width by 2 for double width lines.
    const auto scale = IsDoubleWidthLine(row) ? 1 : 0;
    return GetSize().Width() >> scale;
}

til::point TextBuffer::ClampPositionWithinLine(const til::point position) const
{
    const auto rightmostColumn = GetLineWidth(position.y) - 1;
    return { std::min(position.x, rightmostColumn), position.y };
}

til::point TextBuffer::ScreenToBufferPosition(const til::point position) const
{
    // Use shift right to quickly divide the X pos by 2 for double width lines.
    const auto scale = IsDoubleWidthLine(position.y) ? 1 : 0;
    return { position.x >> scale, position.y };
}

til::point TextBuffer::BufferToScreenPosition(const til::point position) const
{
    // Use shift left to quickly multiply the X pos by 2 for double width lines.
    const auto scale = IsDoubleWidthLine(position.y) ? 1 : 0;
    return { position.x << scale, position.y };
}

// Routine Description:
// - Resets the text contents of this buffer with the default character
//   and the default current color attributes
void TextBuffer::Reset() noexcept
{
    _decommit();
    _initialAttributes = _currentAttributes;
}

// Routine Description:
// - This is the legacy screen resize with minimal changes
// Arguments:
// - newSize - new size of screen.
// Return Value:
// - Success if successful. Invalid parameter if screen buffer size is unexpected. No memory if allocation failed.
void TextBuffer::ResizeTraditional(til::size newSize)
{
    // Guard against resizing the text buffer to 0 columns/rows, which would break being able to insert text.
    newSize.width = std::max(newSize.width, 1);
    newSize.height = std::max(newSize.height, 1);

    TextBuffer newBuffer{ newSize, _currentAttributes, 0, false, _renderer };
    const auto cursorRow = GetCursor().GetPosition().y;
    const auto copyableRows = std::min<til::CoordType>(_height, newSize.height);
    til::CoordType srcRow = 0;
    til::CoordType dstRow = 0;

    if (cursorRow >= newSize.height)
    {
        srcRow = cursorRow - newSize.height + 1;
    }

    for (; dstRow < copyableRows; ++dstRow, ++srcRow)
    {
        newBuffer.GetMutableRowByOffset(dstRow).CopyFrom(GetRowByOffset(srcRow));
    }

    // NOTE: Keep this in sync with _reserve().
    _buffer = std::move(newBuffer._buffer);
    _bufferEnd = newBuffer._bufferEnd;
    _commitWatermark = newBuffer._commitWatermark;
    _initialAttributes = newBuffer._initialAttributes;
    _bufferRowStride = newBuffer._bufferRowStride;
    _bufferOffsetChars = newBuffer._bufferOffsetChars;
    _bufferOffsetCharOffsets = newBuffer._bufferOffsetCharOffsets;
    _width = newBuffer._width;
    _height = newBuffer._height;

    _SetFirstRowIndex(0);
}

void TextBuffer::SetAsActiveBuffer(const bool isActiveBuffer) noexcept
{
    _isActiveBuffer = isActiveBuffer;
}

bool TextBuffer::IsActiveBuffer() const noexcept
{
    return _isActiveBuffer;
}

Microsoft::Console::Render::Renderer& TextBuffer::GetRenderer() noexcept
{
    return _renderer;
}

void TextBuffer::TriggerRedraw(const Viewport& viewport)
{
    if (_isActiveBuffer)
    {
        _renderer.TriggerRedraw(viewport);
    }
}

void TextBuffer::TriggerRedrawCursor(const til::point position)
{
    if (_isActiveBuffer)
    {
        _renderer.TriggerRedrawCursor(&position);
    }
}

void TextBuffer::TriggerRedrawAll()
{
    if (_isActiveBuffer)
    {
        _renderer.TriggerRedrawAll();
    }
}

void TextBuffer::TriggerScroll()
{
    if (_isActiveBuffer)
    {
        _renderer.TriggerScroll();
    }
}

void TextBuffer::TriggerScroll(const til::point delta)
{
    if (_isActiveBuffer)
    {
        _renderer.TriggerScroll(&delta);
    }
}

void TextBuffer::TriggerNewTextNotification(const std::wstring_view newText)
{
    if (_isActiveBuffer)
    {
        _renderer.TriggerNewTextNotification(newText);
    }
}

// Method Description:
// - get delimiter class for buffer cell position
// - used for double click selection and uia word navigation
// Arguments:
// - pos: the buffer cell under observation
// - wordDelimiters: the delimiters defined as a part of the DelimiterClass::DelimiterChar
// Return Value:
// - the delimiter class for the given char
DelimiterClass TextBuffer::_GetDelimiterClassAt(const til::point pos, const std::wstring_view wordDelimiters) const
{
    return GetRowByOffset(pos.y).DelimiterClassAt(pos.x, wordDelimiters);
}

// Method Description:
// - Get the til::point for the beginning of the word you are on
// Arguments:
// - target - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// - accessibilityMode - when enabled, we continue expanding left until we are at the beginning of a readable word.
//                        Otherwise, expand left until a character of a new delimiter class is found
//                        (or a row boundary is encountered)
// - limitOptional - (optional) the last possible position in the buffer that can be explored. This can be used to improve performance.
// Return Value:
// - The til::point for the first character on the "word" (inclusive)
til::point TextBuffer::GetWordStart(const til::point target, const std::wstring_view wordDelimiters, bool accessibilityMode, std::optional<til::point> limitOptional) const
{
    // Consider a buffer with this text in it:
    // "  word   other  "
    // In selection (accessibilityMode = false),
    //  a "word" is defined as the range between two delimiters
    //  so the words in the example include ["  ", "word", "   ", "other", "  "]
    // In accessibility (accessibilityMode = true),
    //  a "word" includes the delimiters after a range of readable characters
    //  so the words in the example include ["word   ", "other  "]
    // NOTE: the start anchor (this one) is inclusive, whereas the end anchor (GetWordEnd) is exclusive

#pragma warning(suppress : 26496)
    auto copy{ target };
    const auto bufferSize{ GetSize() };
    const auto limit{ limitOptional.value_or(bufferSize.EndExclusive()) };
    if (target == bufferSize.Origin())
    {
        // can't expand left
        return target;
    }
    else if (target == bufferSize.EndExclusive())
    {
        // GH#7664: Treat EndExclusive as EndInclusive so
        // that it actually points to a space in the buffer
        copy = bufferSize.BottomRightInclusive();
    }
    else if (bufferSize.CompareInBounds(target, limit, true) >= 0)
    {
        // if at/past the limit --> clamp to limit
        copy = limitOptional.value_or(bufferSize.BottomRightInclusive());
    }

    if (accessibilityMode)
    {
        return _GetWordStartForAccessibility(copy, wordDelimiters);
    }
    else
    {
        return _GetWordStartForSelection(copy, wordDelimiters);
    }
}

// Method Description:
// - Helper method for GetWordStart(). Get the til::point for the beginning of the word (accessibility definition) you are on
// Arguments:
// - target - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// Return Value:
// - The til::point for the first character on the current/previous READABLE "word" (inclusive)
til::point TextBuffer::_GetWordStartForAccessibility(const til::point target, const std::wstring_view wordDelimiters) const
{
    auto result = target;
    const auto bufferSize = GetSize();

    // ignore left boundary. Continue until readable text found
    while (_GetDelimiterClassAt(result, wordDelimiters) != DelimiterClass::RegularChar)
    {
        if (result == bufferSize.Origin())
        {
            //looped around and hit origin (no word between origin and target)
            return result;
        }
        bufferSize.DecrementInBounds(result);
    }

    // make sure we expand to the left boundary or the beginning of the word
    while (_GetDelimiterClassAt(result, wordDelimiters) == DelimiterClass::RegularChar)
    {
        if (result == bufferSize.Origin())
        {
            // first char in buffer is a RegularChar
            // we can't move any further back
            return result;
        }
        bufferSize.DecrementInBounds(result);
    }

    // move off of delimiter
    bufferSize.IncrementInBounds(result);

    return result;
}

// Method Description:
// - Helper method for GetWordStart(). Get the til::point for the beginning of the word (selection definition) you are on
// Arguments:
// - target - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// Return Value:
// - The til::point for the first character on the current word or delimiter run (stopped by the left margin)
til::point TextBuffer::_GetWordStartForSelection(const til::point target, const std::wstring_view wordDelimiters) const
{
    auto result = target;
    const auto bufferSize = GetSize();

    const auto initialDelimiter = _GetDelimiterClassAt(result, wordDelimiters);
    const bool isControlChar = initialDelimiter == DelimiterClass::ControlChar;

    // expand left until we hit the left boundary or a different delimiter class
    while (result != bufferSize.Origin() && _GetDelimiterClassAt(result, wordDelimiters) == initialDelimiter)
    {
        //prevent selection wrapping on whitespace selection
        if (isControlChar && result.x == bufferSize.Left())
        {
            break;
        }
        bufferSize.DecrementInBounds(result);
    }

    if (_GetDelimiterClassAt(result, wordDelimiters) != initialDelimiter)
    {
        // move off of delimiter
        bufferSize.IncrementInBounds(result);
    }

    return result;
}

// Method Description:
// - Get the til::point for the beginning of the NEXT word
// Arguments:
// - target - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// - accessibilityMode - when enabled, we continue expanding right until we are at the beginning of the next READABLE word
//                        Otherwise, expand right until a character of a new delimiter class is found
//                        (or a row boundary is encountered)
// - limitOptional - (optional) the last possible position in the buffer that can be explored. This can be used to improve performance.
// Return Value:
// - The til::point for the last character on the "word" (inclusive)
til::point TextBuffer::GetWordEnd(const til::point target, const std::wstring_view wordDelimiters, bool accessibilityMode, std::optional<til::point> limitOptional) const
{
    // Consider a buffer with this text in it:
    // "  word   other  "
    // In selection (accessibilityMode = false),
    //  a "word" is defined as the range between two delimiters
    //  so the words in the example include ["  ", "word", "   ", "other", "  "]
    // In accessibility (accessibilityMode = true),
    //  a "word" includes the delimiters after a range of readable characters
    //  so the words in the example include ["word   ", "other  "]
    // NOTE: the end anchor (this one) is exclusive, whereas the start anchor (GetWordStart) is inclusive

    // Already at/past the limit. Can't move forward.
    const auto bufferSize{ GetSize() };
    const auto limit{ limitOptional.value_or(bufferSize.EndExclusive()) };
    if (bufferSize.CompareInBounds(target, limit, true) >= 0)
    {
        return target;
    }

    if (accessibilityMode)
    {
        return _GetWordEndForAccessibility(target, wordDelimiters, limit);
    }
    else
    {
        return _GetWordEndForSelection(target, wordDelimiters);
    }
}

// Method Description:
// - Helper method for GetWordEnd(). Get the til::point for the beginning of the next READABLE word
// Arguments:
// - target - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// - limit - the last "valid" position in the text buffer (to improve performance)
// Return Value:
// - The til::point for the first character of the next readable "word". If no next word, return one past the end of the buffer
til::point TextBuffer::_GetWordEndForAccessibility(const til::point target, const std::wstring_view wordDelimiters, const til::point limit) const
{
    const auto bufferSize{ GetSize() };
    auto result{ target };

    if (bufferSize.CompareInBounds(target, limit, true) >= 0)
    {
        // if we're already on/past the last RegularChar,
        // clamp result to that position
        result = limit;

        // make the result exclusive
        bufferSize.IncrementInBounds(result, true);
    }
    else
    {
        while (result != limit && result != bufferSize.BottomRightInclusive() && _GetDelimiterClassAt(result, wordDelimiters) == DelimiterClass::RegularChar)
        {
            // Iterate through readable text
            bufferSize.IncrementInBounds(result);
        }

        while (result != limit && result != bufferSize.BottomRightInclusive() && _GetDelimiterClassAt(result, wordDelimiters) != DelimiterClass::RegularChar)
        {
            // expand to the beginning of the NEXT word
            bufferSize.IncrementInBounds(result);
        }

        // Special case: we tried to move one past the end of the buffer
        // Manually increment onto the EndExclusive point.
        if (result == bufferSize.BottomRightInclusive())
        {
            bufferSize.IncrementInBounds(result, true);
        }
    }

    return result;
}

// Method Description:
// - Helper method for GetWordEnd(). Get the til::point for the beginning of the NEXT word
// Arguments:
// - target - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// Return Value:
// - The til::point for the last character of the current word or delimiter run (stopped by right margin)
til::point TextBuffer::_GetWordEndForSelection(const til::point target, const std::wstring_view wordDelimiters) const
{
    const auto bufferSize = GetSize();

    auto result = target;
    const auto initialDelimiter = _GetDelimiterClassAt(result, wordDelimiters);
    const bool isControlChar = initialDelimiter == DelimiterClass::ControlChar;

    // expand right until we hit the right boundary as a ControlChar or a different delimiter class
    while (result != bufferSize.BottomRightInclusive() && _GetDelimiterClassAt(result, wordDelimiters) == initialDelimiter)
    {
        if (isControlChar && result.x == bufferSize.RightInclusive())
        {
            break;
        }
        bufferSize.IncrementInBoundsCircular(result);
    }

    if (_GetDelimiterClassAt(result, wordDelimiters) != initialDelimiter)
    {
        // move off of delimiter
        bufferSize.DecrementInBounds(result);
    }

    return result;
}

void TextBuffer::_PruneHyperlinks()
{
    // Check the old first row for hyperlink references
    // If there are any, search the entire buffer for the same reference
    // If the buffer does not contain the same reference, we can remove that hyperlink from our map
    // This way, obsolete hyperlink references are cleared from our hyperlink map instead of hanging around
    // Get all the hyperlink references in the row we're erasing
    const auto hyperlinks = GetRowByOffset(0).GetHyperlinks();

    if (!hyperlinks.empty())
    {
        // Move to unordered set so we can use hashed lookup of IDs instead of linear search.
        // Only make it an unordered set now because set always heap allocates but vector
        // doesn't when the set is empty (saving an allocation in the common case of no links.)
        std::unordered_set<uint16_t> firstRowRefs{ hyperlinks.cbegin(), hyperlinks.cend() };

        const auto total = TotalRowCount();
        // Loop through all the rows in the buffer except the first row -
        // we have found all hyperlink references in the first row and put them in refs,
        // now we need to search the rest of the buffer (i.e. all the rows except the first)
        // to see if those references are anywhere else
        for (til::CoordType i = 1; i < total; ++i)
        {
            const auto nextRowRefs = GetRowByOffset(i).GetHyperlinks();
            for (auto id : nextRowRefs)
            {
                if (firstRowRefs.find(id) != firstRowRefs.end())
                {
                    firstRowRefs.erase(id);
                }
            }
            if (firstRowRefs.empty())
            {
                // No more hyperlink references left to search for, terminate early
                break;
            }
        }

        // Now delete obsolete references from our map
        for (auto hyperlinkReference : firstRowRefs)
        {
            RemoveHyperlinkFromMap(hyperlinkReference);
        }
    }
}

// Method Description:
// - Update pos to be the position of the first character of the next word. This is used for accessibility
// Arguments:
// - pos - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// - limitOptional - (optional) the last possible position in the buffer that can be explored. This can be used to improve performance.
// Return Value:
// - true, if successfully updated pos. False, if we are unable to move (usually due to a buffer boundary)
// - pos - The til::point for the first character on the "word" (inclusive)
bool TextBuffer::MoveToNextWord(til::point& pos, const std::wstring_view wordDelimiters, std::optional<til::point> limitOptional) const
{
    // move to the beginning of the next word
    // NOTE: _GetWordEnd...() returns the exclusive position of the "end of the word"
    //       This is also the inclusive start of the next word.
    const auto bufferSize{ GetSize() };
    const auto limit{ limitOptional.value_or(bufferSize.EndExclusive()) };
    const auto copy{ _GetWordEndForAccessibility(pos, wordDelimiters, limit) };

    if (bufferSize.CompareInBounds(copy, limit, true) >= 0)
    {
        return false;
    }

    pos = copy;
    return true;
}

// Method Description:
// - Update pos to be the position of the first character of the previous word. This is used for accessibility
// Arguments:
// - pos - a til::point on the word you are currently on
// - wordDelimiters - what characters are we considering for the separation of words
// Return Value:
// - true, if successfully updated pos. False, if we are unable to move (usually due to a buffer boundary)
// - pos - The til::point for the first character on the "word" (inclusive)
bool TextBuffer::MoveToPreviousWord(til::point& pos, std::wstring_view wordDelimiters) const
{
    // move to the beginning of the current word
    auto copy{ GetWordStart(pos, wordDelimiters, true) };

    if (!GetSize().DecrementInBounds(copy, true))
    {
        // can't move behind current word
        return false;
    }

    // move to the beginning of the previous word
    pos = GetWordStart(copy, wordDelimiters, true);
    return true;
}

// Method Description:
// - Update pos to be the beginning of the current glyph/character. This is used for accessibility
// Arguments:
// - pos - a til::point on the word you are currently on
// - limitOptional - (optional) the last possible position in the buffer that can be explored. This can be used to improve performance.
// Return Value:
// - pos - The til::point for the first cell of the current glyph (inclusive)
til::point TextBuffer::GetGlyphStart(const til::point pos, std::optional<til::point> limitOptional) const
{
    auto resultPos = pos;
    const auto bufferSize = GetSize();
    const auto limit{ limitOptional.value_or(bufferSize.EndExclusive()) };

    // Clamp pos to limit
    if (bufferSize.CompareInBounds(resultPos, limit, true) > 0)
    {
        resultPos = limit;
    }

    // limit is exclusive, so we need to move back to be within valid bounds
    if (resultPos != limit && GetCellDataAt(resultPos)->DbcsAttr() == DbcsAttribute::Trailing)
    {
        bufferSize.DecrementInBounds(resultPos, true);
    }

    return resultPos;
}

// Method Description:
// - Update pos to be the end of the current glyph/character.
// Arguments:
// - pos - a til::point on the word you are currently on
// - accessibilityMode - this is being used for accessibility; make the end exclusive.
// Return Value:
// - pos - The til::point for the last cell of the current glyph (exclusive)
til::point TextBuffer::GetGlyphEnd(const til::point pos, bool accessibilityMode, std::optional<til::point> limitOptional) const
{
    auto resultPos = pos;
    const auto bufferSize = GetSize();
    const auto limit{ limitOptional.value_or(bufferSize.EndExclusive()) };

    // Clamp pos to limit
    if (bufferSize.CompareInBounds(resultPos, limit, true) > 0)
    {
        resultPos = limit;
    }

    if (resultPos != limit && GetCellDataAt(resultPos)->DbcsAttr() == DbcsAttribute::Leading)
    {
        bufferSize.IncrementInBounds(resultPos, true);
    }

    // increment one more time to become exclusive
    if (accessibilityMode)
    {
        bufferSize.IncrementInBounds(resultPos, true);
    }
    return resultPos;
}

// Method Description:
// - Update pos to be the beginning of the next glyph/character. This is used for accessibility
// Arguments:
// - pos - a til::point on the word you are currently on
// - allowExclusiveEnd - allow result to be the exclusive limit (one past limit)
// - limit - boundaries for the iterator to operate within
// Return Value:
// - true, if successfully updated pos. False, if we are unable to move (usually due to a buffer boundary)
// - pos - The til::point for the first cell of the current glyph (inclusive)
bool TextBuffer::MoveToNextGlyph(til::point& pos, bool allowExclusiveEnd, std::optional<til::point> limitOptional) const
{
    const auto bufferSize = GetSize();
    const auto limit{ limitOptional.value_or(bufferSize.EndExclusive()) };

    const auto distanceToLimit{ bufferSize.CompareInBounds(pos, limit, true) };
    if (distanceToLimit >= 0)
    {
        // Corner Case: we're on/past the limit
        // Clamp us to the limit
        pos = limit;
        return false;
    }
    else if (!allowExclusiveEnd && distanceToLimit == -1)
    {
        // Corner Case: we're just before the limit
        // and we are not allowed onto the exclusive end.
        // Fail to move.
        return false;
    }

    // Try to move forward, but if we hit the buffer boundary, we fail to move.
    auto iter{ GetCellDataAt(pos, bufferSize) };
    const bool success{ ++iter };

    // Move again if we're on a wide glyph
    if (success && iter->DbcsAttr() == DbcsAttribute::Trailing)
    {
        ++iter;
    }

    pos = iter.Pos();
    return success;
}

// Method Description:
// - Update pos to be the beginning of the previous glyph/character. This is used for accessibility
// Arguments:
// - pos - a til::point on the word you are currently on
// Return Value:
// - true, if successfully updated pos. False, if we are unable to move (usually due to a buffer boundary)
// - pos - The til::point for the first cell of the previous glyph (inclusive)
bool TextBuffer::MoveToPreviousGlyph(til::point& pos, std::optional<til::point> limitOptional) const
{
    auto resultPos = pos;
    const auto bufferSize = GetSize();
    const auto limit{ limitOptional.value_or(bufferSize.EndExclusive()) };

    if (bufferSize.CompareInBounds(pos, limit, true) > 0)
    {
        // we're past the end
        // clamp us to the limit
        pos = limit;
        return true;
    }

    // try to move. If we can't, we're done.
    const auto success = bufferSize.DecrementInBounds(resultPos, true);
    if (resultPos != bufferSize.EndExclusive() && GetCellDataAt(resultPos)->DbcsAttr() == DbcsAttribute::Leading)
    {
        bufferSize.DecrementInBounds(resultPos, true);
    }

    pos = resultPos;
    return success;
}

// Method Description:
// - Determines the line-by-line rectangles based on two COORDs
// - expands the rectangles to support wide glyphs
// - used for selection rects and UIA bounding rects
// Arguments:
// - start: a corner of the text region of interest (inclusive)
// - end: the other corner of the text region of interest (inclusive)
// - blockSelection: when enabled, only get the rectangular text region,
//                   as opposed to the text extending to the left/right
//                   buffer margins
// - bufferCoordinates: when enabled, treat the coordinates as relative to
//                      the buffer rather than the screen.
// Return Value:
// - One or more rects corresponding to the selection area
const std::vector<til::inclusive_rect> TextBuffer::GetTextRects(til::point start, til::point end, bool blockSelection, bool bufferCoordinates) const
{
    std::vector<til::inclusive_rect> textRects;

    const auto bufferSize = GetSize();

    // (0,0) is the top-left of the screen
    // the physically "higher" coordinate is closer to the top-left
    // the physically "lower" coordinate is closer to the bottom-right
    const auto [higherCoord, lowerCoord] = bufferSize.CompareInBounds(start, end) <= 0 ?
                                               std::make_tuple(start, end) :
                                               std::make_tuple(end, start);

    const auto textRectSize = 1 + lowerCoord.y - higherCoord.y;
    textRects.reserve(textRectSize);
    for (auto row = higherCoord.y; row <= lowerCoord.y; row++)
    {
        til::inclusive_rect textRow;

        textRow.top = row;
        textRow.bottom = row;

        if (blockSelection || higherCoord.y == lowerCoord.y)
        {
            // set the left and right margin to the left-/right-most respectively
            textRow.left = std::min(higherCoord.x, lowerCoord.x);
            textRow.right = std::max(higherCoord.x, lowerCoord.x);
        }
        else
        {
            textRow.left = (row == higherCoord.y) ? higherCoord.x : bufferSize.Left();
            textRow.right = (row == lowerCoord.y) ? lowerCoord.x : bufferSize.RightInclusive();
        }

        // If we were passed screen coordinates, convert the given range into
        // equivalent buffer offsets, taking line rendition into account.
        if (!bufferCoordinates)
        {
            textRow = ScreenToBufferLine(textRow, GetLineRendition(row));
        }

        _ExpandTextRow(textRow);
        textRects.emplace_back(textRow);
    }

    return textRects;
}

// Method Description:
// - Computes the span(s) for the given selection
// - If not a blockSelection, returns a single span (start - end)
// - Else if a blockSelection, returns spans corresponding to each line in the block selection
// Arguments:
// - start: beginning of the text region of interest (inclusive)
// - end: the other end of the text region of interest (inclusive)
// - blockSelection: when enabled, get spans for each line covered by the block
// - bufferCoordinates: when enabled, treat the coordinates as relative to
//                      the buffer rather than the screen.
// Return Value:
// - one or more sets of start-end coordinates, representing spans of text in the buffer
std::vector<til::point_span> TextBuffer::GetTextSpans(til::point start, til::point end, bool blockSelection, bool bufferCoordinates) const
{
    std::vector<til::point_span> textSpans;
    if (blockSelection)
    {
        // If blockSelection, this is effectively the same operation as GetTextRects, but
        // expressed in til::point coordinates.
        const auto rects = GetTextRects(start, end, /*blockSelection*/ true, bufferCoordinates);
        textSpans.reserve(rects.size());

        for (auto rect : rects)
        {
            const til::point first = { rect.left, rect.top };
            const til::point second = { rect.right, rect.bottom };
            textSpans.emplace_back(first, second);
        }
    }
    else
    {
        const auto bufferSize = GetSize();

        // (0,0) is the top-left of the screen
        // the physically "higher" coordinate is closer to the top-left
        // the physically "lower" coordinate is closer to the bottom-right
        auto [higherCoord, lowerCoord] = start <= end ?
                                             std::make_tuple(start, end) :
                                             std::make_tuple(end, start);

        textSpans.reserve(1);

        // If we were passed screen coordinates, convert the given range into
        // equivalent buffer offsets, taking line rendition into account.
        if (!bufferCoordinates)
        {
            higherCoord = ScreenToBufferLine(higherCoord, GetLineRendition(higherCoord.y));
            lowerCoord = ScreenToBufferLine(lowerCoord, GetLineRendition(lowerCoord.y));
        }

        til::inclusive_rect asRect = { higherCoord.x, higherCoord.y, lowerCoord.x, lowerCoord.y };
        _ExpandTextRow(asRect);
        higherCoord.x = asRect.left;
        higherCoord.y = asRect.top;
        lowerCoord.x = asRect.right;
        lowerCoord.y = asRect.bottom;

        textSpans.emplace_back(higherCoord, lowerCoord);
    }

    return textSpans;
}

// Method Description:
// - Expand the selection row according to include wide glyphs fully
// - this is particularly useful for box selections (ALT + selection)
// Arguments:
// - selectionRow: the selection row to be expanded
// Return Value:
// - modifies selectionRow's Left and Right values to expand properly
void TextBuffer::_ExpandTextRow(til::inclusive_rect& textRow) const
{
    const auto bufferSize = GetSize();

    // expand left side of rect
    til::point targetPoint{ textRow.left, textRow.top };
    if (GetCellDataAt(targetPoint)->DbcsAttr() == DbcsAttribute::Trailing)
    {
        if (targetPoint.x == bufferSize.Left())
        {
            bufferSize.IncrementInBounds(targetPoint);
        }
        else
        {
            bufferSize.DecrementInBounds(targetPoint);
        }
        textRow.left = targetPoint.x;
    }

    // expand right side of rect
    targetPoint = { textRow.right, textRow.bottom };
    if (GetCellDataAt(targetPoint)->DbcsAttr() == DbcsAttribute::Leading)
    {
        if (targetPoint.x == bufferSize.RightInclusive())
        {
            bufferSize.DecrementInBounds(targetPoint);
        }
        else
        {
            bufferSize.IncrementInBounds(targetPoint);
        }
        textRow.right = targetPoint.x;
    }
}

// Routine Description:
// - Retrieves the text data from the selected region and presents it in a clipboard-ready format (given little post-processing).
// Arguments:
// - includeCRLF - inject CRLF pairs to the end of each line
// - trimTrailingWhitespace - remove the trailing whitespace at the end of each line
// - textRects - the rectangular regions from which the data will be extracted from the buffer (i.e.: selection rects)
// - GetAttributeColors - function used to map TextAttribute to RGB COLORREFs. If null, only extract the text.
// - formatWrappedRows - if set we will apply formatting (CRLF inclusion and whitespace trimming) on wrapped rows
// Return Value:
// - The text, background color, and foreground color data of the selected region of the text buffer.
const TextBuffer::TextAndColor TextBuffer::GetText(const bool includeCRLF,
                                                   const bool trimTrailingWhitespace,
                                                   const std::vector<til::inclusive_rect>& selectionRects,
                                                   std::function<std::pair<COLORREF, COLORREF>(const TextAttribute&)> GetAttributeColors,
                                                   const bool formatWrappedRows) const
{
    TextAndColor data;
    const auto copyTextColor = GetAttributeColors != nullptr;

    // preallocate our vectors to reduce reallocs
    const auto rows = selectionRects.size();
    data.text.reserve(rows);
    if (copyTextColor)
    {
        data.FgAttr.reserve(rows);
        data.BkAttr.reserve(rows);
    }

    // for each row in the selection
    for (size_t i = 0; i < rows; i++)
    {
        const auto iRow = selectionRects.at(i).top;

        const auto highlight = Viewport::FromInclusive(selectionRects.at(i));

        // retrieve the data from the screen buffer
        auto it = GetCellDataAt(highlight.Origin(), highlight);

        // allocate a string buffer
        std::wstring selectionText;
        std::vector<COLORREF> selectionFgAttr;
        std::vector<COLORREF> selectionBkAttr;

        // preallocate to avoid reallocs
        selectionText.reserve(gsl::narrow<size_t>(highlight.Width()) + 2); // + 2 for \r\n if we munged it
        if (copyTextColor)
        {
            selectionFgAttr.reserve(gsl::narrow<size_t>(highlight.Width()) + 2);
            selectionBkAttr.reserve(gsl::narrow<size_t>(highlight.Width()) + 2);
        }

        // copy char data into the string buffer, skipping trailing bytes
        while (it)
        {
            const auto& cell = *it;

            if (cell.DbcsAttr() != DbcsAttribute::Trailing)
            {
                const auto chars = cell.Chars();
                selectionText.append(chars);

                if (copyTextColor)
                {
                    const auto cellData = cell.TextAttr();
                    const auto [CellFgAttr, CellBkAttr] = GetAttributeColors(cellData);
                    for (size_t j = 0; j < chars.size(); ++j)
                    {
                        selectionFgAttr.push_back(CellFgAttr);
                        selectionBkAttr.push_back(CellBkAttr);
                    }
                }
            }

            ++it;
        }

        // We apply formatting to rows if the row was NOT wrapped or formatting of wrapped rows is allowed
        const auto shouldFormatRow = formatWrappedRows || !GetRowByOffset(iRow).WasWrapForced();

        if (trimTrailingWhitespace)
        {
            if (shouldFormatRow)
            {
                // remove the spaces at the end (aka trim the trailing whitespace)
                while (!selectionText.empty() && selectionText.back() == UNICODE_SPACE)
                {
                    selectionText.pop_back();
                    if (copyTextColor)
                    {
                        selectionFgAttr.pop_back();
                        selectionBkAttr.pop_back();
                    }
                }
            }
        }

        // apply CR/LF to the end of the final string, unless we're the last line.
        // a.k.a if we're earlier than the bottom, then apply CR/LF.
        if (includeCRLF && i < selectionRects.size() - 1)
        {
            if (shouldFormatRow)
            {
                // then we can assume a CR/LF is proper
                selectionText.push_back(UNICODE_CARRIAGERETURN);
                selectionText.push_back(UNICODE_LINEFEED);

                if (copyTextColor)
                {
                    // can't see CR/LF so just use black FG & BK
                    const auto Blackness = RGB(0x00, 0x00, 0x00);
                    selectionFgAttr.push_back(Blackness);
                    selectionFgAttr.push_back(Blackness);
                    selectionBkAttr.push_back(Blackness);
                    selectionBkAttr.push_back(Blackness);
                }
            }
        }

        data.text.emplace_back(std::move(selectionText));
        if (copyTextColor)
        {
            data.FgAttr.emplace_back(std::move(selectionFgAttr));
            data.BkAttr.emplace_back(std::move(selectionBkAttr));
        }
    }

    return data;
}

size_t TextBuffer::SpanLength(const til::point coordStart, const til::point coordEnd) const
{
    const auto bufferSize = GetSize();
    // The coords are inclusive, so to get the (inclusive) length we add 1.
    const auto length = bufferSize.CompareInBounds(coordEnd, coordStart) + 1;
    return gsl::narrow<size_t>(length);
}

// Routine Description:
// - Retrieves the plain text data between the specified coordinates.
// Arguments:
// - trimTrailingWhitespace - remove the trailing whitespace at the end of the result.
// - start - where to start getting text (should be at or prior to "end")
// - end - where to end getting text
// Return Value:
// - Just the text.
std::wstring TextBuffer::GetPlainText(const til::point& start, const til::point& end) const
{
    std::wstring text;
    auto spanLength = SpanLength(start, end);
    text.reserve(spanLength);

    auto it = GetCellDataAt(start);

    for (; it && spanLength > 0; ++it, --spanLength)
    {
        const auto& cell = *it;
        if (cell.DbcsAttr() != DbcsAttribute::Trailing)
        {
            const auto chars = cell.Chars();
            text.append(chars);
        }
    }

    return text;
}

// Routine Description:
// - Generates a CF_HTML compliant structure based on the passed in text and color data
// Arguments:
// - rows - the text and color data we will format & encapsulate
// - backgroundColor - default background color for characters, also used in padding
// - fontHeightPoints - the unscaled font height
// - fontFaceName - the name of the font used
// Return Value:
// - string containing the generated HTML
std::string TextBuffer::GenHTML(const TextAndColor& rows,
                                const int fontHeightPoints,
                                const std::wstring_view fontFaceName,
                                const COLORREF backgroundColor)
{
    try
    {
        std::ostringstream htmlBuilder;

        // First we have to add some standard
        // HTML boiler plate required for CF_HTML
        // as part of the HTML Clipboard format
        const std::string htmlHeader =
            "<!DOCTYPE><HTML><HEAD></HEAD><BODY>";
        htmlBuilder << htmlHeader;

        htmlBuilder << "<!--StartFragment -->";

        // apply global style in div element
        {
            htmlBuilder << "<DIV STYLE=\"";
            htmlBuilder << "display:inline-block;";
            htmlBuilder << "white-space:pre;";

            htmlBuilder << "background-color:";
            htmlBuilder << Utils::ColorToHexString(backgroundColor);
            htmlBuilder << ";";

            htmlBuilder << "font-family:";
            htmlBuilder << "'";
            htmlBuilder << ConvertToA(CP_UTF8, fontFaceName);
            htmlBuilder << "',";
            // even with different font, add monospace as fallback
            htmlBuilder << "monospace;";

            htmlBuilder << "font-size:";
            htmlBuilder << fontHeightPoints;
            htmlBuilder << "pt;";

            // note: MS Word doesn't support padding (in this way at least)
            htmlBuilder << "padding:";
            htmlBuilder << 4; // todo: customizable padding
            htmlBuilder << "px;";

            htmlBuilder << "\">";
        }

        // copy text and info color from buffer
        auto hasWrittenAnyText = false;
        std::optional<COLORREF> fgColor = std::nullopt;
        std::optional<COLORREF> bkColor = std::nullopt;
        for (size_t row = 0; row < rows.text.size(); row++)
        {
            size_t startOffset = 0;

            if (row != 0)
            {
                htmlBuilder << "<BR>";
            }

            for (size_t col = 0; col < rows.text.at(row).length(); col++)
            {
                const auto writeAccumulatedChars = [&](bool includeCurrent) {
                    if (col >= startOffset)
                    {
                        const auto unescapedText = ConvertToA(CP_UTF8, std::wstring_view(rows.text.at(row)).substr(startOffset, col - startOffset + includeCurrent));
                        for (const auto c : unescapedText)
                        {
                            switch (c)
                            {
                            case '<':
                                htmlBuilder << "&lt;";
                                break;
                            case '>':
                                htmlBuilder << "&gt;";
                                break;
                            case '&':
                                htmlBuilder << "&amp;";
                                break;
                            default:
                                htmlBuilder << c;
                            }
                        }

                        startOffset = col;
                    }
                };

                if (rows.text.at(row).at(col) == '\r' || rows.text.at(row).at(col) == '\n')
                {
                    // do not include \r nor \n as they don't have color attributes
                    // and are not HTML friendly. For line break use '<BR>' instead.
                    writeAccumulatedChars(false);
                    break;
                }

                auto colorChanged = false;
                if (!fgColor.has_value() || rows.FgAttr.at(row).at(col) != fgColor.value())
                {
                    fgColor = rows.FgAttr.at(row).at(col);
                    colorChanged = true;
                }

                if (!bkColor.has_value() || rows.BkAttr.at(row).at(col) != bkColor.value())
                {
                    bkColor = rows.BkAttr.at(row).at(col);
                    colorChanged = true;
                }

                if (colorChanged)
                {
                    writeAccumulatedChars(false);

                    if (hasWrittenAnyText)
                    {
                        htmlBuilder << "</SPAN>";
                    }

                    htmlBuilder << "<SPAN STYLE=\"";
                    htmlBuilder << "color:";
                    htmlBuilder << Utils::ColorToHexString(fgColor.value());
                    htmlBuilder << ";";
                    htmlBuilder << "background-color:";
                    htmlBuilder << Utils::ColorToHexString(bkColor.value());
                    htmlBuilder << ";";
                    htmlBuilder << "\">";
                }

                hasWrittenAnyText = true;

                // if this is the last character in the row, flush the whole row
                if (col == rows.text.at(row).length() - 1)
                {
                    writeAccumulatedChars(true);
                }
            }
        }

        if (hasWrittenAnyText)
        {
            // last opened span wasn't closed in loop above, so close it now
            htmlBuilder << "</SPAN>";
        }

        htmlBuilder << "</DIV>";

        htmlBuilder << "<!--EndFragment -->";

        constexpr std::string_view HtmlFooter = "</BODY></HTML>";
        htmlBuilder << HtmlFooter;

        // once filled with values, there will be exactly 157 bytes in the clipboard header
        constexpr size_t ClipboardHeaderSize = 157;

        // these values are byte offsets from start of clipboard
        const auto htmlStartPos = ClipboardHeaderSize;
        const auto htmlEndPos = ClipboardHeaderSize + gsl::narrow<size_t>(htmlBuilder.tellp());
        const auto fragStartPos = ClipboardHeaderSize + gsl::narrow<size_t>(htmlHeader.length());
        const auto fragEndPos = htmlEndPos - HtmlFooter.length();

        // header required by HTML 0.9 format
        std::ostringstream clipHeaderBuilder;
        clipHeaderBuilder << "Version:0.9\r\n";
        clipHeaderBuilder << std::setfill('0');
        clipHeaderBuilder << "StartHTML:" << std::setw(10) << htmlStartPos << "\r\n";
        clipHeaderBuilder << "EndHTML:" << std::setw(10) << htmlEndPos << "\r\n";
        clipHeaderBuilder << "StartFragment:" << std::setw(10) << fragStartPos << "\r\n";
        clipHeaderBuilder << "EndFragment:" << std::setw(10) << fragEndPos << "\r\n";
        clipHeaderBuilder << "StartSelection:" << std::setw(10) << fragStartPos << "\r\n";
        clipHeaderBuilder << "EndSelection:" << std::setw(10) << fragEndPos << "\r\n";

        return clipHeaderBuilder.str() + htmlBuilder.str();
    }
    catch (...)
    {
        LOG_HR(wil::ResultFromCaughtException());
        return {};
    }
}

// Routine Description:
// - Generates an RTF document based on the passed in text and color data
//   RTF 1.5 Spec: https://www.biblioscape.com/rtf15_spec.htm
//   RTF 1.9.1 Spec: https://msopenspecs.azureedge.net/files/Archive_References/[MSFT-RTF].pdf
// Arguments:
// - rows - the text and color data we will format & encapsulate
// - backgroundColor - default background color for characters, also used in padding
// - fontHeightPoints - the unscaled font height
// - fontFaceName - the name of the font used
// - htmlTitle - value used in title tag of html header. Used to name the application
// Return Value:
// - string containing the generated RTF
std::string TextBuffer::GenRTF(const TextAndColor& rows, const int fontHeightPoints, const std::wstring_view fontFaceName, const COLORREF backgroundColor)
{
    try
    {
        std::ostringstream rtfBuilder;

        // start rtf
        rtfBuilder << "{";

        // Standard RTF header.
        // This is similar to the header generated by WordPad.
        // \ansi:
        //   Specifies that the ANSI char set is used in the current doc.
        // \ansicpg1252:
        //   Represents the ANSI code page which is used to perform
        //   the Unicode to ANSI conversion when writing RTF text.
        // \deff0:
        //   Specifies that the default font for the document is the one
        //   at index 0 in the font table.
        // \nouicompat:
        //   Some features are blocked by default to maintain compatibility
        //   with older programs (Eg. Word 97-2003). `nouicompat` disables this
        //   behavior, and unblocks these features. See: Spec 1.9.1, Pg. 51.
        rtfBuilder << "\\rtf1\\ansi\\ansicpg1252\\deff0\\nouicompat";

        // font table
        rtfBuilder << "{\\fonttbl{\\f0\\fmodern\\fcharset0 " << ConvertToA(CP_UTF8, fontFaceName) << ";}}";

        // map to keep track of colors:
        // keys are colors represented by COLORREF
        // values are indices of the corresponding colors in the color table
        std::unordered_map<COLORREF, size_t> colorMap;

        // RTF color table
        std::ostringstream colorTableBuilder;
        colorTableBuilder << "{\\colortbl ;";

        const auto getColorTableIndex = [&](const COLORREF color) -> size_t {
            // Exclude the 0 index for the default color, and start with 1.

            const auto [it, inserted] = colorMap.emplace(color, colorMap.size() + 1);
            if (inserted)
            {
                colorTableBuilder << "\\red" << static_cast<int>(GetRValue(color))
                                  << "\\green" << static_cast<int>(GetGValue(color))
                                  << "\\blue" << static_cast<int>(GetBValue(color))
                                  << ";";
            }
            return it->second;
        };

        // content
        std::ostringstream contentBuilder;
        contentBuilder << "\\viewkind4\\uc4";

        // paragraph styles
        // \fs specifies font size in half-points i.e. \fs20 results in a font size
        // of 10 pts. That's why, font size is multiplied by 2 here.
        contentBuilder << "\\pard\\slmult1\\f0\\fs" << std::to_string(2 * fontHeightPoints)
                       // Set the background color for the page. But, the
                       // standard way (\cbN) to do this isn't supported in Word.
                       // However, the following control words sequence works
                       // in Word (and other RTF editors also) for applying the
                       // text background color. See: Spec 1.9.1, Pg. 23.
                       << "\\chshdng0\\chcbpat" << getColorTableIndex(backgroundColor)
                       << " ";

        std::optional<COLORREF> fgColor = std::nullopt;
        std::optional<COLORREF> bkColor = std::nullopt;
        for (size_t row = 0; row < rows.text.size(); ++row)
        {
            size_t startOffset = 0;

            if (row != 0)
            {
                contentBuilder << "\\line "; // new line
            }

            for (size_t col = 0; col < rows.text.at(row).length(); ++col)
            {
                const auto writeAccumulatedChars = [&](bool includeCurrent) {
                    if (col >= startOffset)
                    {
                        const auto text = std::wstring_view{ rows.text.at(row) }.substr(startOffset, col - startOffset + includeCurrent);
                        _AppendRTFText(contentBuilder, text);

                        startOffset = col;
                    }
                };

                if (rows.text.at(row).at(col) == '\r' || rows.text.at(row).at(col) == '\n')
                {
                    // do not include \r nor \n as they don't have color attributes.
                    // For line break use \line instead.
                    writeAccumulatedChars(false);
                    break;
                }

                auto colorChanged = false;
                if (!fgColor.has_value() || rows.FgAttr.at(row).at(col) != fgColor.value())
                {
                    fgColor = rows.FgAttr.at(row).at(col);
                    colorChanged = true;
                }

                if (!bkColor.has_value() || rows.BkAttr.at(row).at(col) != bkColor.value())
                {
                    bkColor = rows.BkAttr.at(row).at(col);
                    colorChanged = true;
                }

                if (colorChanged)
                {
                    writeAccumulatedChars(false);
                    contentBuilder << "\\chshdng0\\chcbpat" << getColorTableIndex(bkColor.value())
                                   << "\\cf" << getColorTableIndex(fgColor.value())
                                   << " ";
                }

                // if this is the last character in the row, flush the whole row
                if (col == rows.text.at(row).length() - 1)
                {
                    writeAccumulatedChars(true);
                }
            }
        }

        // end colortbl
        colorTableBuilder << "}";

        // add color table to the final RTF
        rtfBuilder << colorTableBuilder.str();

        // add the text content to the final RTF
        rtfBuilder << contentBuilder.str();

        // end rtf
        rtfBuilder << "}";

        return rtfBuilder.str();
    }
    catch (...)
    {
        LOG_HR(wil::ResultFromCaughtException());
        return {};
    }
}

void TextBuffer::_AppendRTFText(std::ostringstream& contentBuilder, const std::wstring_view& text)
{
    for (const auto codeUnit : text)
    {
        if (codeUnit <= 127)
        {
            switch (codeUnit)
            {
            case L'\\':
            case L'{':
            case L'}':
                contentBuilder << "\\" << gsl::narrow<char>(codeUnit);
                break;
            default:
                contentBuilder << gsl::narrow<char>(codeUnit);
            }
        }
        else
        {
            // Windows uses unsigned wchar_t - RTF uses signed ones.
            contentBuilder << "\\u" << std::to_string(til::bit_cast<int16_t>(codeUnit)) << "?";
        }
    }
}

// Function Description:
// - Reflow the contents from the old buffer into the new buffer. The new buffer
//   can have different dimensions than the old buffer. If it does, then this
//   function will attempt to maintain the logical contents of the old buffer,
//   by continuing wrapped lines onto the next line in the new buffer.
// Arguments:
// - oldBuffer - the text buffer to copy the contents FROM
// - newBuffer - the text buffer to copy the contents TO
// - lastCharacterViewport - Optional. If the caller knows that the last
//   nonspace character is in a particular Viewport, the caller can provide this
//   parameter as an optimization, as opposed to searching the entire buffer.
// - positionInfo - Optional. The caller can provide a pair of rows in this
//   parameter and we'll calculate the position of the _end_ of those rows in
//   the new buffer. The rows's new value is placed back into this parameter.
// Return Value:
// - S_OK if we successfully copied the contents to the new buffer, otherwise an appropriate HRESULT.
void TextBuffer::Reflow(TextBuffer& oldBuffer, TextBuffer& newBuffer, const Viewport* lastCharacterViewport, PositionInformation* positionInfo)
{
    const auto& oldCursor = oldBuffer.GetCursor();
    auto& newCursor = newBuffer.GetCursor();

    til::point oldCursorPos = oldCursor.GetPosition();
    til::point newCursorPos;

    // BODGY: We use oldCursorPos in two critical places below:
    // * To compute an oldHeight that includes at a minimum the cursor row
    // * For REFLOW_JANK_CURSOR_WRAP (see comment below)
    // Both of these would break the reflow algorithm, but the latter of the two in particular
    // would cause the main copy loop below to deadlock. In other words, these two lines
    // protect this function against yet-unknown bugs in other parts of the code base.
    oldCursorPos.x = std::clamp(oldCursorPos.x, 0, oldBuffer._width - 1);
    oldCursorPos.y = std::clamp(oldCursorPos.y, 0, oldBuffer._height - 1);

    const auto lastRowWithText = oldBuffer.GetLastNonSpaceCharacter(lastCharacterViewport).y;

    auto mutableViewportTop = positionInfo ? positionInfo->mutableViewportTop : til::CoordTypeMax;
    auto visibleViewportTop = positionInfo ? positionInfo->visibleViewportTop : til::CoordTypeMax;

    til::CoordType oldY = 0;
    til::CoordType newY = 0;
    til::CoordType newX = 0;
    til::CoordType newWidth = newBuffer.GetSize().Width();
    til::CoordType newYLimit = til::CoordTypeMax;

    const auto oldHeight = std::max(lastRowWithText, oldCursorPos.y) + 1;
    const auto newHeight = newBuffer.GetSize().Height();
    const auto newWidthU16 = gsl::narrow_cast<uint16_t>(newWidth);

    // Copy oldBuffer into newBuffer until oldBuffer has been fully consumed.
    for (; oldY < oldHeight && newY < newYLimit; ++oldY)
    {
        const auto& oldRow = oldBuffer.GetRowByOffset(oldY);

        // A pair of double height rows should optimally wrap as a union (i.e. after wrapping there should be 4 lines).
        // But for this initial implementation I chose the alternative approach: Just truncate them.
        if (oldRow.GetLineRendition() != LineRendition::SingleWidth)
        {
            // Since rows with a non-standard line rendition should be truncated it's important
            // that we pretend as if the previous row ended in a newline, even if it didn't.
            // This is what this if does: It newlines.
            if (newX)
            {
                newX = 0;
                newY++;
            }

            auto& newRow = newBuffer.GetMutableRowByOffset(newY);

            // See the comment marked with "REFLOW_RESET".
            if (newY >= newHeight)
            {
                newRow.Reset(newBuffer._initialAttributes);
            }

            newRow.CopyFrom(oldRow);
            newRow.SetWrapForced(false);

            if (oldY == oldCursorPos.y)
            {
                newCursorPos = { newRow.AdjustToGlyphStart(oldCursorPos.x), newY };
            }
            if (oldY >= mutableViewportTop)
            {
                positionInfo->mutableViewportTop = newY;
                mutableViewportTop = til::CoordTypeMax;
            }
            if (oldY >= visibleViewportTop)
            {
                positionInfo->visibleViewportTop = newY;
                visibleViewportTop = til::CoordTypeMax;
            }

            newY++;
            continue;
        }

        // Rows don't store any information for what column the last written character is in.
        // We simply truncate all trailing whitespace in this implementation.
        auto oldRowLimit = oldRow.MeasureRight();
        if (oldY == oldCursorPos.y)
        {
            // REFLOW_JANK_CURSOR_WRAP:
            // Pretending as if there's always at least whitespace in front of the cursor has the benefit that
            // * the cursor retains its distance from any preceding text.
            // * when a client application starts writing on this new, empty line,
            //   enlarging the buffer unwraps the text onto the preceding line.
            oldRowLimit = std::max(oldRowLimit, oldCursorPos.x + 1);
        }

        til::CoordType oldX = 0;

        // Copy oldRow into newBuffer until oldRow has been fully consumed.
        // We use a do-while loop to ensure that line wrapping occurs and
        // that attributes are copied over even for seemingly empty rows.
        do
        {
            // This if condition handles line wrapping.
            // Only if we write past the last column we should wrap and as such this if
            // condition is in front of the text insertion code instead of behind it.
            // A SetWrapForced of false implies an explicit newline, which is the default.
            if (newX >= newWidth)
            {
                newBuffer.GetMutableRowByOffset(newY).SetWrapForced(true);
                newX = 0;
                newY++;
            }

            // REFLOW_RESET:
            // If we shrink the buffer vertically, for instance from 100 rows to 90 rows, we will write 10 rows in the
            // new buffer twice. We need to reset them before copying text, or otherwise we'll see the previous contents.
            // We don't need to be smart about this. Reset() is fast and shrinking doesn't occur often.
            if (newY >= newHeight && newX == 0)
            {
                // We need to ensure not to overwrite the row the cursor is on.
                if (newY >= newYLimit)
                {
                    break;
                }
                newBuffer.GetMutableRowByOffset(newY).Reset(newBuffer._initialAttributes);
            }

            auto& newRow = newBuffer.GetMutableRowByOffset(newY);

            RowCopyTextFromState state{
                .source = oldRow,
                .columnBegin = newX,
                .columnLimit = til::CoordTypeMax,
                .sourceColumnBegin = oldX,
                .sourceColumnLimit = oldRowLimit,
            };
            newRow.CopyTextFrom(state);

            const auto& oldAttr = oldRow.Attributes();
            auto& newAttr = newRow.Attributes();
            const auto attributes = oldAttr.slice(gsl::narrow_cast<uint16_t>(oldX), oldAttr.size());
            newAttr.replace(gsl::narrow_cast<uint16_t>(newX), newAttr.size(), attributes);
            newAttr.resize_trailing_extent(newWidthU16);

            if (oldY == oldCursorPos.y && oldCursorPos.x >= oldX)
            {
                // In theory AdjustToGlyphStart ensures we don't put the cursor on a trailing wide glyph.
                // In practice I don't think that this can possibly happen. Better safe than sorry.
                newCursorPos = { newRow.AdjustToGlyphStart(oldCursorPos.x - oldX + newX), newY };
                // If there's so much text past the old cursor position that it doesn't fit into new buffer,
                // then the new cursor position will be "lost", because it's overwritten by unrelated text.
                // We have two choices how can handle this:
                // * If the new cursor is at an y < 0, just put the cursor at (0,0)
                // * Stop writing into the new buffer before we overwrite the new cursor position
                // This implements the second option. There's no fundamental reason why this is better.
                newYLimit = newY + newHeight;
            }
            if (oldY >= mutableViewportTop)
            {
                positionInfo->mutableViewportTop = newY;
                mutableViewportTop = til::CoordTypeMax;
            }
            if (oldY >= visibleViewportTop)
            {
                positionInfo->visibleViewportTop = newY;
                visibleViewportTop = til::CoordTypeMax;
            }

            oldX = state.sourceColumnEnd;
            newX = state.columnEnd;
        } while (oldX < oldRowLimit);

        // If the row had an explicit newline we also need to newline. :)
        if (!oldRow.WasWrapForced())
        {
            newX = 0;
            newY++;
        }
    }

    // Finish copying buffer attributes to remaining rows below the last
    // printable character. This is to fix the `color 2f` scenario, where you
    // change the buffer colors then resize and everything below the last
    // printable char gets reset. See GH #12567
    const auto initializedRowsEnd = oldBuffer._estimateOffsetOfLastCommittedRow() + 1;
    for (; oldY < initializedRowsEnd && newY < newHeight; oldY++, newY++)
    {
        auto& oldRow = oldBuffer.GetRowByOffset(oldY);
        auto& newRow = newBuffer.GetMutableRowByOffset(newY);
        auto& newAttr = newRow.Attributes();
        newAttr = oldRow.Attributes();
        newAttr.resize_trailing_extent(newWidthU16);
    }

    // Since we didn't use IncrementCircularBuffer() we need to compute the proper
    // _firstRow offset now, in a way that replicates IncrementCircularBuffer().
    // We need to do the same for newCursorPos.y for basically the same reason.
    if (newY > newHeight)
    {
        newBuffer._firstRow = newY % newHeight;
        // _firstRow maps from API coordinates that always start at 0,0 in the top left corner of the
        // terminal's scrollback, to the underlying buffer Y coordinate via `(y + _firstRow) % height`.
        // Here, we need to un-map the `newCursorPos.y` from the underlying Y coordinate to the API coordinate
        // and so we do `(y - _firstRow) % height`, but we add `+ newHeight` to avoid getting negative results.
        newCursorPos.y = (newCursorPos.y - newBuffer._firstRow + newHeight) % newHeight;
    }

    newBuffer.CopyProperties(oldBuffer);
    newBuffer.CopyHyperlinkMaps(oldBuffer);

    assert(newCursorPos.x >= 0 && newCursorPos.x < newWidth);
    assert(newCursorPos.y >= 0 && newCursorPos.y < newHeight);
    newCursor.SetSize(oldCursor.GetSize());
    newCursor.SetPosition(newCursorPos);

    newBuffer._marks = oldBuffer._marks;
    newBuffer._trimMarksOutsideBuffer();
}

// Method Description:
// - Adds or updates a hyperlink in our hyperlink table
// Arguments:
// - The hyperlink URI, the hyperlink id (could be new or old)
void TextBuffer::AddHyperlinkToMap(std::wstring_view uri, uint16_t id)
{
    _hyperlinkMap[id] = uri;
}

// Method Description:
// - Retrieves the URI associated with a particular hyperlink ID
// Arguments:
// - The hyperlink ID
// Return Value:
// - The URI
std::wstring TextBuffer::GetHyperlinkUriFromId(uint16_t id) const
{
    return _hyperlinkMap.at(id);
}

// Method description:
// - Provides the hyperlink ID to be assigned as a text attribute, based on the optional custom id provided
// Arguments:
// - The user-defined id
// Return value:
// - The internal hyperlink ID
uint16_t TextBuffer::GetHyperlinkId(std::wstring_view uri, std::wstring_view id)
{
    uint16_t numericId = 0;
    if (id.empty())
    {
        // no custom id specified, return our internal count
        numericId = _currentHyperlinkId;
        ++_currentHyperlinkId;
    }
    else
    {
        // assign _currentHyperlinkId if the custom id does not already exist
        std::wstring newId{ id };
        // hash the URL and add it to the custom ID - GH#7698
        newId += L"%" + std::to_wstring(til::hash(uri));
        const auto result = _hyperlinkCustomIdMap.emplace(newId, _currentHyperlinkId);
        if (result.second)
        {
            // the custom id did not already exist
            ++_currentHyperlinkId;
        }
        numericId = (*(result.first)).second;
    }
    // _currentHyperlinkId could overflow, make sure its not 0
    if (_currentHyperlinkId == 0)
    {
        ++_currentHyperlinkId;
    }
    return numericId;
}

// Method Description:
// - Removes a hyperlink from the hyperlink map and the associated
//   user defined id from the custom id map (if there is one)
// Arguments:
// - The ID of the hyperlink to be removed
void TextBuffer::RemoveHyperlinkFromMap(uint16_t id) noexcept
{
    _hyperlinkMap.erase(id);
    for (const auto& customIdPair : _hyperlinkCustomIdMap)
    {
        if (customIdPair.second == id)
        {
            _hyperlinkCustomIdMap.erase(customIdPair.first);
            break;
        }
    }
}

// Method Description:
// - Obtains the custom ID, if there was one, associated with the
//   uint16_t id of a hyperlink
// Arguments:
// - The uint16_t id of the hyperlink
// Return Value:
// - The custom ID if there was one, empty string otherwise
std::wstring TextBuffer::GetCustomIdFromId(uint16_t id) const
{
    for (auto customIdPair : _hyperlinkCustomIdMap)
    {
        if (customIdPair.second == id)
        {
            return customIdPair.first;
        }
    }
    return {};
}

// Method Description:
// - Copies the hyperlink/customID maps of the old buffer into this one,
//   also copies currentHyperlinkId
// Arguments:
// - The other buffer
void TextBuffer::CopyHyperlinkMaps(const TextBuffer& other)
{
    _hyperlinkMap = other._hyperlinkMap;
    _hyperlinkCustomIdMap = other._hyperlinkCustomIdMap;
    _currentHyperlinkId = other._currentHyperlinkId;
}

// Searches through the entire (committed) text buffer for `needle` and returns the coordinates in absolute coordinates.
// The end coordinates of the returned ranges are considered inclusive.
std::vector<til::point_span> TextBuffer::SearchText(const std::wstring_view& needle, bool caseInsensitive) const
{
    return SearchText(needle, caseInsensitive, 0, til::CoordTypeMax);
}

// Searches through the given rows [rowBeg,rowEnd) for `needle` and returns the coordinates in absolute coordinates.
// While the end coordinates of the returned ranges are considered inclusive, the [rowBeg,rowEnd) range is half-open.
std::vector<til::point_span> TextBuffer::SearchText(const std::wstring_view& needle, bool caseInsensitive, til::CoordType rowBeg, til::CoordType rowEnd) const
{
    rowEnd = std::min(rowEnd, _estimateOffsetOfLastCommittedRow() + 1);

    std::vector<til::point_span> results;

    // All whitespace strings would match the not-yet-written parts of the TextBuffer which would be weird.
    if (allWhitespace(needle) || rowBeg >= rowEnd)
    {
        return results;
    }

    auto text = ICU::UTextFromTextBuffer(*this, rowBeg, rowEnd);

    uint32_t flags = UREGEX_LITERAL;
    WI_SetFlagIf(flags, UREGEX_CASE_INSENSITIVE, caseInsensitive);

    UErrorCode status = U_ZERO_ERROR;
    const auto re = ICU::CreateRegex(needle, flags, &status);
    uregex_setUText(re.get(), &text, &status);

    if (uregex_find(re.get(), -1, &status))
    {
        do
        {
            results.emplace_back(ICU::BufferRangeFromMatch(&text, re.get()));
        } while (uregex_findNext(re.get(), &status));
    }

    return results;
}

const std::vector<ScrollMark>& TextBuffer::GetMarks() const noexcept
{
    return _marks;
}

// Remove all marks between `start` & `end`, inclusive.
void TextBuffer::ClearMarksInRange(
    const til::point start,
    const til::point end)
{
    auto inRange = [&start, &end](const ScrollMark& m) {
        return (m.start >= start && m.start <= end) ||
               (m.end >= start && m.end <= end);
    };

    _marks.erase(std::remove_if(_marks.begin(),
                                _marks.end(),
                                inRange),
                 _marks.end());
}
void TextBuffer::ClearAllMarks() noexcept
{
    _marks.clear();
}

// Adjust all the marks in the y-direction by `delta`. Positive values move the
// marks down (the positive y direction). Negative values move up. This will
// trim marks that are no longer have a start in the bounds of the buffer
void TextBuffer::ScrollMarks(const int delta)
{
    for (auto& mark : _marks)
    {
        mark.start.y += delta;

        // If the mark had sub-regions, then move those pointers too
        if (mark.commandEnd.has_value())
        {
            (*mark.commandEnd).y += delta;
        }
        if (mark.outputEnd.has_value())
        {
            (*mark.outputEnd).y += delta;
        }
    }
    _trimMarksOutsideBuffer();
}

// Method Description:
// - Add a mark to our list of marks, and treat it as the active "prompt". For
//   the sake of shell integration, we need to know which mark represents the
//   current prompt/command/output. Internally, we'll always treat the _last_
//   mark in the list as the current prompt.
// Arguments:
// - m: the mark to add.
void TextBuffer::StartPromptMark(const ScrollMark& m)
{
    _marks.push_back(m);
}
// Method Description:
// - Add a mark to our list of marks. Don't treat this as the active prompt.
//   This should be used for marks created by the UI or from other user input.
//   By inserting at the start of the list, we can separate out marks that were
//   generated by client programs vs ones created by the user.
// Arguments:
// - m: the mark to add.
void TextBuffer::AddMark(const ScrollMark& m)
{
    _marks.insert(_marks.begin(), m);
}

void TextBuffer::_trimMarksOutsideBuffer()
{
    const til::CoordType height = _height;
    std::erase_if(_marks, [height](const auto& m) {
        return (m.start.y < 0) || (m.start.y >= height);
    });
}

std::wstring_view TextBuffer::CurrentCommand() const
{
    if (_marks.size() == 0)
    {
        return L"";
    }

    const auto& curr{ _marks.back() };
    const auto& start{ curr.end };
    const auto& end{ GetCursor().GetPosition() };

    const auto line = start.y;
    const auto& row = GetRowByOffset(line);
    return row.GetText(start.x, end.x);
}

void TextBuffer::SetCurrentPromptEnd(const til::point pos) noexcept
{
    if (_marks.empty())
    {
        return;
    }
    auto& curr{ _marks.back() };
    curr.end = pos;
}
void TextBuffer::SetCurrentCommandEnd(const til::point pos) noexcept
{
    if (_marks.empty())
    {
        return;
    }
    auto& curr{ _marks.back() };
    curr.commandEnd = pos;
}
void TextBuffer::SetCurrentOutputEnd(const til::point pos, ::MarkCategory category) noexcept
{
    if (_marks.empty())
    {
        return;
    }
    auto& curr{ _marks.back() };
    curr.outputEnd = pos;
    curr.category = category;
}
