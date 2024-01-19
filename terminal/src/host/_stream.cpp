// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "ApiRoutines.h"

#include "_stream.h"
#include "stream.h"
#include "writeData.hpp"

#include "_output.h"
#include "output.h"
#include "dbcs.h"
#include "handle.h"
#include "misc.h"

#include "../types/inc/convert.hpp"
#include "../types/inc/GlyphWidth.hpp"
#include "../types/inc/Viewport.hpp"

#include "../interactivity/inc/ServiceLocator.hpp"

#pragma hdrstop
using namespace Microsoft::Console::Types;
using Microsoft::Console::Interactivity::ServiceLocator;
using Microsoft::Console::VirtualTerminal::StateMachine;
// Used by WriteCharsLegacy.
#define IS_GLYPH_CHAR(wch) (((wch) >= L' ') && ((wch) != 0x007F))

// Routine Description:
// - This routine updates the cursor position.  Its input is the non-special
//   cased new location of the cursor.  For example, if the cursor were being
//   moved one space backwards from the left edge of the screen, the X
//   coordinate would be -1.  This routine would set the X coordinate to
//   the right edge of the screen and decrement the Y coordinate by one.
// Arguments:
// - screenInfo - reference to screen buffer information structure.
// - coordCursor - New location of cursor.
// - fKeepCursorVisible - TRUE if changing window origin desirable when hit right edge
// Return Value:
static void AdjustCursorPosition(SCREEN_INFORMATION& screenInfo, _In_ til::point coordCursor, _Inout_opt_ til::CoordType* psScrollY)
{
    const auto bufferSize = screenInfo.GetBufferSize().Dimensions();
    if (coordCursor.x < 0)
    {
        if (coordCursor.y > 0)
        {
            coordCursor.x = bufferSize.width + coordCursor.x;
            coordCursor.y = coordCursor.y - 1;
        }
        else
        {
            coordCursor.x = 0;
        }
    }
    else if (coordCursor.x >= bufferSize.width)
    {
        // at end of line. if wrap mode, wrap cursor.  otherwise leave it where it is.
        if (screenInfo.OutputMode & ENABLE_WRAP_AT_EOL_OUTPUT)
        {
            coordCursor.y += coordCursor.x / bufferSize.width;
            coordCursor.x = coordCursor.x % bufferSize.width;
        }
        else
        {
            coordCursor.x = screenInfo.GetTextBuffer().GetCursor().GetPosition().x;
        }
    }

    if (coordCursor.y >= bufferSize.height)
    {
        auto& buffer = screenInfo.GetTextBuffer();
        buffer.IncrementCircularBuffer(buffer.GetCurrentAttributes());

        if (buffer.IsActiveBuffer())
        {
            if (const auto notifier = ServiceLocator::LocateAccessibilityNotifier())
            {
                notifier->NotifyConsoleUpdateScrollEvent(0, -1);
            }
            if (const auto renderer = ServiceLocator::LocateGlobals().pRender)
            {
                static constexpr til::point delta{ 0, -1 };
                renderer->TriggerScroll(&delta);
            }
        }

        if (psScrollY)
        {
            *psScrollY += 1;
        }

        coordCursor.y = bufferSize.height - 1;
    }

    const auto cursorMovedPastViewport = coordCursor.y > screenInfo.GetViewport().BottomInclusive();

    // if at right or bottom edge of window, scroll right or down one char.
    if (cursorMovedPastViewport)
    {
        til::point WindowOrigin;
        WindowOrigin.x = 0;
        WindowOrigin.y = coordCursor.y - screenInfo.GetViewport().BottomInclusive();
        LOG_IF_FAILED(screenInfo.SetViewportOrigin(false, WindowOrigin, true));
    }

    LOG_IF_FAILED(screenInfo.SetCursorPosition(coordCursor, false));
}

// As the name implies, this writes text without processing its control characters.
void _writeCharsLegacyUnprocessed(SCREEN_INFORMATION& screenInfo, const std::wstring_view& text, til::CoordType* psScrollY)
{
    const auto wrapAtEOL = WI_IsFlagSet(screenInfo.OutputMode, ENABLE_WRAP_AT_EOL_OUTPUT);
    const auto hasAccessibilityEventing = screenInfo.HasAccessibilityEventing();
    auto& textBuffer = screenInfo.GetTextBuffer();

    RowWriteState state{
        .text = text,
        .columnLimit = textBuffer.GetSize().RightExclusive(),
    };

    while (!state.text.empty())
    {
        auto cursorPosition = textBuffer.GetCursor().GetPosition();

        state.columnBegin = cursorPosition.x;
        textBuffer.Write(cursorPosition.y, textBuffer.GetCurrentAttributes(), state);
        cursorPosition.x = state.columnEnd;

        if (wrapAtEOL && state.columnEnd >= state.columnLimit)
        {
            textBuffer.SetWrapForced(cursorPosition.y, true);
        }

        if (hasAccessibilityEventing && state.columnEnd > state.columnBegin)
        {
            screenInfo.NotifyAccessibilityEventing(state.columnBegin, cursorPosition.y, state.columnEnd - 1, cursorPosition.y);
        }

        AdjustCursorPosition(screenInfo, cursorPosition, psScrollY);
    }
}

// This routine writes a string to the screen while handling control characters.
// `interactive` exists for COOKED_READ_DATA which uses it to transform control characters into visible text like "^X".
// Similarly, `psScrollY` is also used by it to track whether the underlying buffer circled. It requires this information to know where the input line moved to.
void WriteCharsLegacy(SCREEN_INFORMATION& screenInfo, const std::wstring_view& text, til::CoordType* psScrollY)
{
    static constexpr wchar_t tabSpaces[8]{ L' ', L' ', L' ', L' ', L' ', L' ', L' ', L' ' };

    auto& textBuffer = screenInfo.GetTextBuffer();
    const auto width = textBuffer.GetSize().Width();
    auto& cursor = textBuffer.GetCursor();
    const auto wrapAtEOL = WI_IsFlagSet(screenInfo.OutputMode, ENABLE_WRAP_AT_EOL_OUTPUT);
    auto it = text.begin();
    const auto end = text.end();

    // In VT mode, when you have a 120-column terminal you can write 120 columns without the cursor wrapping.
    // Whenever the cursor is in that 120th column IsDelayedEOLWrap() will return true. I'm not sure why the VT parts
    // of the code base store this as a boolean. It's also unclear why we handle this here. The intention is likely
    // so that when we exit VT mode and receive a write a potentially stored delayed wrap would still be handled.
    // The way this code does it however isn't correct since it handles it like the old console APIs would and
    // so writing a newline while being delay wrapped will print 2 newlines.
    if (cursor.IsDelayedEOLWrap() && wrapAtEOL)
    {
        auto pos = cursor.GetPosition();
        const auto delayed = cursor.GetDelayedAtPosition();
        cursor.ResetDelayEOLWrap();
        if (delayed == pos)
        {
            pos.x = 0;
            pos.y++;
            AdjustCursorPosition(screenInfo, pos, psScrollY);
        }
    }

    // If ENABLE_PROCESSED_OUTPUT is set we search for C0 control characters and handle them like backspace, tab, etc.
    // If it's not set, we can just straight up give everything to WriteCharsLegacyUnprocessed.
    if (WI_IsFlagClear(screenInfo.OutputMode, ENABLE_PROCESSED_OUTPUT))
    {
        _writeCharsLegacyUnprocessed(screenInfo, { it, end }, psScrollY);
        it = end;
    }

    while (it != end)
    {
        const auto nextControlChar = std::find_if(it, end, [](const auto& wch) { return !IS_GLYPH_CHAR(wch); });
        if (nextControlChar != it)
        {
            _writeCharsLegacyUnprocessed(screenInfo, { it, nextControlChar }, psScrollY);
            it = nextControlChar;
        }

        for (; it != end && !IS_GLYPH_CHAR(*it); ++it)
        {
            switch (*it)
            {
            case UNICODE_NULL:
                _writeCharsLegacyUnprocessed(screenInfo, { &tabSpaces[0], 1 }, psScrollY);
                continue;
            case UNICODE_BELL:
                std::ignore = screenInfo.SendNotifyBeep();
                continue;
            case UNICODE_BACKSPACE:
            {
                auto pos = cursor.GetPosition();
                pos.x = textBuffer.GetRowByOffset(pos.y).NavigateToPrevious(pos.x);
                AdjustCursorPosition(screenInfo, pos, psScrollY);
                continue;
            }
            case UNICODE_TAB:
            {
                const auto pos = cursor.GetPosition();
                const auto remaining = width - pos.x;
                const auto tabCount = gsl::narrow_cast<size_t>(std::min(remaining, 8 - (pos.x & 7)));
                _writeCharsLegacyUnprocessed(screenInfo, { &tabSpaces[0], tabCount }, psScrollY);
                continue;
            }
            case UNICODE_LINEFEED:
            {
                auto pos = cursor.GetPosition();
                if (WI_IsFlagClear(screenInfo.OutputMode, DISABLE_NEWLINE_AUTO_RETURN))
                {
                    pos.x = 0;
                }

                textBuffer.GetMutableRowByOffset(pos.y).SetWrapForced(false);
                pos.y = pos.y + 1;
                AdjustCursorPosition(screenInfo, pos, psScrollY);
                continue;
            }
            case UNICODE_CARRIAGERETURN:
            {
                auto pos = cursor.GetPosition();
                pos.x = 0;
                AdjustCursorPosition(screenInfo, pos, psScrollY);
                continue;
            }
            default:
                break;
            }

            // As a special favor to incompetent apps that attempt to display control chars,
            // convert to corresponding OEM Glyph Chars
            const auto cp = ServiceLocator::LocateGlobals().getConsoleInformation().OutputCP;
            const auto ch = gsl::narrow_cast<char>(*it);
            wchar_t wch = 0;
            const auto result = MultiByteToWideChar(cp, MB_USEGLYPHCHARS, &ch, 1, &wch, 1);
            if (result == 1)
            {
                _writeCharsLegacyUnprocessed(screenInfo, { &wch, 1 }, psScrollY);
            }
        }
    }
}

// Routine Description:
// - Takes the given text and inserts it into the given screen buffer.
// Note:
// - Console lock must be held when calling this routine
// - String has been translated to unicode at this point.
// Arguments:
// - pwchBuffer - wide character text to be inserted into buffer
// - pcbBuffer - byte count of pwchBuffer on the way in, number of bytes consumed on the way out.
// - screenInfo - Screen Information class to write the text into at the current cursor position
// - ppWaiter - If writing to the console is blocked for whatever reason, this will be filled with a pointer to context
//              that can be used by the server to resume the call at a later time.
// Return Value:
// - STATUS_SUCCESS if OK.
// - CONSOLE_STATUS_WAIT if we couldn't finish now and need to be called back later (see ppWaiter).
// - Or a suitable NTSTATUS format error code for memory/string/math failures.
[[nodiscard]] NTSTATUS DoWriteConsole(_In_reads_bytes_(*pcbBuffer) PCWCHAR pwchBuffer,
                                      _Inout_ size_t* const pcbBuffer,
                                      SCREEN_INFORMATION& screenInfo,
                                      bool requiresVtQuirk,
                                      std::unique_ptr<WriteData>& waiter)
try
{
    const auto& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    if (WI_IsAnyFlagSet(gci.Flags, (CONSOLE_SUSPENDED | CONSOLE_SELECTING | CONSOLE_SCROLLBAR_TRACKING)))
    {
        waiter = std::make_unique<WriteData>(screenInfo,
                                             pwchBuffer,
                                             *pcbBuffer,
                                             gci.OutputCP,
                                             requiresVtQuirk);
        return CONSOLE_STATUS_WAIT;
    }

    const auto vtIo = ServiceLocator::LocateGlobals().getConsoleInformation().GetVtIo();
    const auto restoreVtQuirk = wil::scope_exit([&]() {
        if (requiresVtQuirk)
        {
            screenInfo.ResetIgnoreLegacyEquivalentVTAttributes();
        }
        if (vtIo->IsUsingVt())
        {
            vtIo->CorkRenderer(false);
        }
    });
    if (requiresVtQuirk)
    {
        screenInfo.SetIgnoreLegacyEquivalentVTAttributes();
    }
    if (vtIo->IsUsingVt())
    {
        vtIo->CorkRenderer(true);
    }

    const std::wstring_view str{ pwchBuffer, *pcbBuffer / sizeof(WCHAR) };

    if (WI_IsAnyFlagClear(screenInfo.OutputMode, ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT))
    {
        WriteCharsLegacy(screenInfo, str, nullptr);
    }
    else
    {
        screenInfo.GetStateMachine().ProcessString(str);
    }

    return STATUS_SUCCESS;
}
NT_CATCH_RETURN()

// Routine Description:
// - This method performs the actual work of attempting to write to the console, converting data types as necessary
//   to adapt from the server types to the legacy internal host types.
// - It operates on Unicode data only. It's assumed the text is translated by this point.
// Arguments:
// - OutContext - the console output object to write the new text into
// - pwsTextBuffer - wide character text buffer provided by client application to insert
// - cchTextBufferLength - text buffer counted in characters
// - pcchTextBufferRead - character count of the number of characters we were able to insert before returning
// - ppWaiter - If we are blocked from writing now and need to wait, this is filled with contextual data for the server to restore the call later
// Return Value:
// - S_OK if successful.
// - S_OK if we need to wait (check if ppWaiter is not nullptr).
// - Or a suitable HRESULT code for math/string/memory failures.
[[nodiscard]] HRESULT WriteConsoleWImplHelper(IConsoleOutputObject& context,
                                              const std::wstring_view buffer,
                                              size_t& read,
                                              bool requiresVtQuirk,
                                              std::unique_ptr<WriteData>& waiter) noexcept
{
    try
    {
        // Set out variables in case we exit early.
        read = 0;
        waiter.reset();

        // Convert characters to bytes to give to DoWriteConsole.
        size_t cbTextBufferLength;
        RETURN_IF_FAILED(SizeTMult(buffer.size(), sizeof(wchar_t), &cbTextBufferLength));

        auto Status = DoWriteConsole(const_cast<wchar_t*>(buffer.data()), &cbTextBufferLength, context, requiresVtQuirk, waiter);

        // Convert back from bytes to characters for the resulting string length written.
        read = cbTextBufferLength / sizeof(wchar_t);

        if (Status == CONSOLE_STATUS_WAIT)
        {
            FAIL_FAST_IF_NULL(waiter.get());
            Status = STATUS_SUCCESS;
        }

        RETURN_NTSTATUS(Status);
    }
    CATCH_RETURN();
}

// Routine Description:
// - Writes non-Unicode formatted data into the given console output object.
// - This method will convert from the given input into wide characters before chain calling the wide character version of the function.
//   It uses the current Output Codepage for conversions (set via SetConsoleOutputCP).
// - NOTE: This may be blocked for various console states and will return a wait context pointer if necessary.
// Arguments:
// - context - the console output object to write the new text into
// - buffer - char/byte text buffer provided by client application to insert
// - read - character count of the number of characters (also bytes because A version) we were able to insert before returning
// - waiter - If we are blocked from writing now and need to wait, this is filled with contextual data for the server to restore the call later
// Return Value:
// - S_OK if successful.
// - S_OK if we need to wait (check if ppWaiter is not nullptr).
// - Or a suitable HRESULT code for math/string/memory failures.
[[nodiscard]] HRESULT ApiRoutines::WriteConsoleAImpl(IConsoleOutputObject& context,
                                                     const std::string_view buffer,
                                                     size_t& read,
                                                     bool requiresVtQuirk,
                                                     std::unique_ptr<IWaitRoutine>& waiter) noexcept
{
    try
    {
        // Ensure output variables are initialized.
        read = 0;
        waiter.reset();

        if (buffer.empty())
        {
            return S_OK;
        }

        LockConsole();
        auto unlock{ wil::scope_exit([&] { UnlockConsole(); }) };

        auto& screenInfo{ context.GetActiveBuffer() };
        const auto& consoleInfo{ ServiceLocator::LocateGlobals().getConsoleInformation() };
        const auto codepage{ consoleInfo.OutputCP };
        auto leadByteCaptured{ false };
        auto leadByteConsumed{ false };
        std::wstring wstr{};
        static til::u8state u8State{};

        // Convert our input parameters to Unicode
        if (codepage == CP_UTF8)
        {
            RETURN_IF_FAILED(til::u8u16(buffer, wstr, u8State));
            read = buffer.size();
        }
        else
        {
            // In case the codepage changes from UTF-8 to another,
            // we discard partials that might still be cached.
            u8State.reset();

            int mbPtrLength{};
            RETURN_IF_FAILED(SizeTToInt(buffer.size(), &mbPtrLength));

            // (buffer.size() + 2) I think because we might be shoving another unicode char
            // from screenInfo->WriteConsoleDbcsLeadByte in front
            // because we previously checked that buffer.size() fits into an int, +2 won't cause an overflow of size_t
            wstr.resize(buffer.size() + 2);

            auto wcPtr{ wstr.data() };
            auto mbPtr{ buffer.data() };
            size_t dbcsLength{};
            if (screenInfo.WriteConsoleDbcsLeadByte[0] != 0 && gsl::narrow_cast<byte>(*mbPtr) >= byte{ ' ' })
            {
                // there was a portion of a dbcs character stored from a previous
                // call so we take the 2nd half from mbPtr[0], put them together
                // and write the wide char to wcPtr[0]
                screenInfo.WriteConsoleDbcsLeadByte[1] = gsl::narrow_cast<byte>(*mbPtr);

                try
                {
                    const auto wFromComplemented{
                        ConvertToW(codepage, { reinterpret_cast<const char*>(screenInfo.WriteConsoleDbcsLeadByte), ARRAYSIZE(screenInfo.WriteConsoleDbcsLeadByte) })
                    };

                    FAIL_FAST_IF(wFromComplemented.size() != 1);
                    dbcsLength = sizeof(wchar_t);
                    wcPtr[0] = wFromComplemented.at(0);
                    mbPtr++;
                }
                catch (...)
                {
                    dbcsLength = 0;
                }

                // this looks weird to be always incrementing even if the conversion failed, but this is the
                // original behavior so it's left unchanged.
                wcPtr++;
                mbPtrLength--;

                // Note that we used a stored lead byte from a previous call in order to complete this write
                // Use this to offset the "number of bytes consumed" calculation at the end by -1 to account
                // for using a byte we had internally, not off the stream.
                leadByteConsumed = true;
            }

            screenInfo.WriteConsoleDbcsLeadByte[0] = 0;

            // if the last byte in mbPtr is a lead byte for the current code page,
            // save it for the next time this function is called and we can piece it
            // back together then
            if (mbPtrLength != 0 && CheckBisectStringA(const_cast<char*>(mbPtr), mbPtrLength, &consoleInfo.OutputCPInfo))
            {
                screenInfo.WriteConsoleDbcsLeadByte[0] = gsl::narrow_cast<byte>(mbPtr[mbPtrLength - 1]);
                mbPtrLength--;

                // Note that we captured a lead byte during this call, but won't actually draw it until later.
                // Use this to offset the "number of bytes consumed" calculation at the end by +1 to account
                // for taking a byte off the stream.
                leadByteCaptured = true;
            }

            if (mbPtrLength != 0)
            {
                // convert the remaining bytes in mbPtr to wide chars
                mbPtrLength = sizeof(wchar_t) * MultiByteToWideChar(codepage, 0, mbPtr, mbPtrLength, wcPtr, mbPtrLength);
            }

            wstr.resize((dbcsLength + mbPtrLength) / sizeof(wchar_t));
        }

        // Hold the specific version of the waiter locally so we can tinker with it if we have to store additional context.
        std::unique_ptr<WriteData> writeDataWaiter{};

        // Make the W version of the call
        size_t wcBufferWritten{};
        const auto hr{ WriteConsoleWImplHelper(screenInfo, wstr, wcBufferWritten, requiresVtQuirk, writeDataWaiter) };

        // If there is no waiter, process the byte count now.
        if (nullptr == writeDataWaiter.get())
        {
            // Calculate how many bytes of the original A buffer were consumed in the W version of the call to satisfy mbBufferRead.
            // For UTF-8 conversions, we've already returned this information above.
            if (CP_UTF8 != codepage)
            {
                size_t mbBufferRead{};

                // Start by counting the number of A bytes we used in printing our W string to the screen.
                try
                {
                    mbBufferRead = GetALengthFromW(codepage, { wstr.data(), wcBufferWritten });
                }
                CATCH_LOG();

                // If we captured a byte off the string this time around up above, it means we didn't feed
                // it into the WriteConsoleW above, and therefore its consumption isn't accounted for
                // in the count we just made. Add +1 to compensate.
                if (leadByteCaptured)
                {
                    mbBufferRead++;
                }

                // If we consumed an internally-stored lead byte this time around up above, it means that we
                // fed a byte into WriteConsoleW that wasn't a part of this particular call's request.
                // We need to -1 to compensate and tell the caller the right number of bytes consumed this request.
                if (leadByteConsumed)
                {
                    mbBufferRead--;
                }

                read = mbBufferRead;
            }
        }
        else
        {
            // If there is a waiter, then we need to stow some additional information in the wait structure so
            // we can synthesize the correct byte count later when the wait routine is triggered.
            if (CP_UTF8 != codepage)
            {
                // For non-UTF8 codepages, save the lead byte captured/consumed data so we can +1 or -1 the final decoded count
                // in the WaitData::Notify method later.
                writeDataWaiter->SetLeadByteAdjustmentStatus(leadByteCaptured, leadByteConsumed);
            }
            else
            {
                // For UTF8 codepages, just remember the consumption count from the UTF-8 parser.
                writeDataWaiter->SetUtf8ConsumedCharacters(read);
            }
        }

        // Give back the waiter now that we're done with tinkering with it.
        waiter.reset(writeDataWaiter.release());

        return hr;
    }
    CATCH_RETURN();
}

// Routine Description:
// - Writes Unicode formatted data into the given console output object.
// - NOTE: This may be blocked for various console states and will return a wait context pointer if necessary.
// Arguments:
// - OutContext - the console output object to write the new text into
// - pwsTextBuffer - wide character text buffer provided by client application to insert
// - cchTextBufferLength - text buffer counted in characters
// - pcchTextBufferRead - character count of the number of characters we were able to insert before returning
// - ppWaiter - If we are blocked from writing now and need to wait, this is filled with contextual data for the server to restore the call later
// Return Value:
// - S_OK if successful.
// - S_OK if we need to wait (check if ppWaiter is not nullptr).
// - Or a suitable HRESULT code for math/string/memory failures.
[[nodiscard]] HRESULT ApiRoutines::WriteConsoleWImpl(IConsoleOutputObject& context,
                                                     const std::wstring_view buffer,
                                                     size_t& read,
                                                     bool requiresVtQuirk,
                                                     std::unique_ptr<IWaitRoutine>& waiter) noexcept
{
    try
    {
        LockConsole();
        auto unlock = wil::scope_exit([&] { UnlockConsole(); });

        std::unique_ptr<WriteData> writeDataWaiter;
        RETURN_IF_FAILED(WriteConsoleWImplHelper(context.GetActiveBuffer(), buffer, read, requiresVtQuirk, writeDataWaiter));

        // Transfer specific waiter pointer into the generic interface wrapper.
        waiter.reset(writeDataWaiter.release());

        return S_OK;
    }
    CATCH_RETURN();
}
