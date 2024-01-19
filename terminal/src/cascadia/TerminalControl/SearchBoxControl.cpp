// Copyright (c) Microsoft Corporation
// Licensed under the MIT license.

#include "pch.h"
#include "SearchBoxControl.h"
#include "SearchBoxControl.g.cpp"
#include <LibraryResources.h>

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Core;

namespace winrt::Microsoft::Terminal::Control::implementation
{
    // Constructor
    SearchBoxControl::SearchBoxControl()
    {
        InitializeComponent();

        this->CharacterReceived({ this, &SearchBoxControl::_CharacterHandler });
        this->KeyDown({ this, &SearchBoxControl::_KeyDownHandler });
        this->RegisterPropertyChangedCallback(UIElement::VisibilityProperty(), [this](auto&&, auto&&) {
            // Once the control is visible again we trigger SearchChanged event.
            // We do this since we probably have a value from the previous search,
            // and in such case logically the search changes from "nothing" to this value.
            // A good example for SearchChanged event consumer is Terminal Control.
            // Once the Search Box is open we want the Terminal Control
            // to immediately perform the search with the value appearing in the box.
            if (Visibility() == Visibility::Visible)
            {
                _SearchChangedHandlers(TextBox().Text(), _GoForward(), _CaseSensitive());
            }
        });

        _focusableElements.insert(TextBox());
        _focusableElements.insert(CloseButton());
        _focusableElements.insert(CaseSensitivityButton());
        _focusableElements.insert(GoForwardButton());
        _focusableElements.insert(GoBackwardButton());

        StatusBox().Width(_GetStatusMaxWidth());
    }

    // Method Description:
    // - Check if the current search direction is forward
    // Arguments:
    // - <none>
    // Return Value:
    // - bool: the current search direction, determined by the
    //         states of the two direction buttons
    bool SearchBoxControl::_GoForward()
    {
        return GoForwardButton().IsChecked().GetBoolean();
    }

    // Method Description:
    // - Check if the current search is case sensitive
    // Arguments:
    // - <none>
    // Return Value:
    // - bool: whether the current search is case sensitive (case button is checked )
    //   or not
    bool SearchBoxControl::_CaseSensitive()
    {
        return CaseSensitivityButton().IsChecked().GetBoolean();
    }

    // Method Description:
    // - Handler for pressing Enter on TextBox, trigger
    //   text search
    // Arguments:
    // - sender: not used
    // - e: event data
    // Return Value:
    // - <none>
    void SearchBoxControl::TextBoxKeyDown(const winrt::Windows::Foundation::IInspectable& /*sender*/, const Input::KeyRoutedEventArgs& e)
    {
        if (e.OriginalKey() == winrt::Windows::System::VirtualKey::Enter)
        {
            // If the buttons are disabled, then don't allow enter to search either.
            if (!GoForwardButton().IsEnabled() || !GoBackwardButton().IsEnabled())
            {
                return;
            }

            const auto state = CoreWindow::GetForCurrentThread().GetKeyState(winrt::Windows::System::VirtualKey::Shift);
            if (WI_IsFlagSet(state, CoreVirtualKeyStates::Down))
            {
                _SearchHandlers(TextBox().Text(), !_GoForward(), _CaseSensitive());
            }
            else
            {
                _SearchHandlers(TextBox().Text(), _GoForward(), _CaseSensitive());
            }
            e.Handled(true);
        }
    }

    // Method Description:
    // - Handler for pressing "Esc" when focusing
    //   on the search dialog, this triggers close
    //   event of the Search dialog
    // Arguments:
    // - sender: not used
    // - e: event data
    // Return Value:
    // - <none>
    void SearchBoxControl::_KeyDownHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                           const Input::KeyRoutedEventArgs& e)
    {
        if (e.OriginalKey() == winrt::Windows::System::VirtualKey::Escape)
        {
            _ClosedHandlers(*this, e);
            e.Handled(true);
        }
    }

    // Method Description:
    // - Handler for pressing Enter on TextBox, trigger
    //   text search
    // Arguments:
    // - <none>
    // Return Value:
    // - <none>
    void SearchBoxControl::SetFocusOnTextbox()
    {
        if (TextBox())
        {
            Input::FocusManager::TryFocusAsync(TextBox(), FocusState::Keyboard);
            TextBox().SelectAll();
        }
    }

    // Method Description:
    // - Allows to set the value of the text to search
    // Arguments:
    // - text: string value to populate in the TextBox
    // Return Value:
    // - <none>
    void SearchBoxControl::PopulateTextbox(const winrt::hstring& text)
    {
        if (TextBox())
        {
            TextBox().Text(text);
        }
    }

    // Method Description:
    // - Check if the current focus is on any element within the
    //   search box
    // Arguments:
    // - <none>
    // Return Value:
    // - bool: whether the current focus is on the search box
    bool SearchBoxControl::ContainsFocus()
    {
        auto focusedElement = Input::FocusManager::GetFocusedElement(this->XamlRoot());
        if (_focusableElements.count(focusedElement) > 0)
        {
            return true;
        }

        return false;
    }

    // Method Description:
    // - Handler for clicking the GoBackward button. This change the value of _goForward,
    //   mark GoBackward button as checked and ensure GoForward button
    //   is not checked
    // Arguments:
    // - sender: not used
    // - e: not used
    // Return Value:
    // - <none>
    void SearchBoxControl::GoBackwardClicked(const winrt::Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        GoBackwardButton().IsChecked(true);
        if (GoForwardButton().IsChecked())
        {
            GoForwardButton().IsChecked(false);
        }

        // kick off search
        _SearchHandlers(TextBox().Text(), _GoForward(), _CaseSensitive());
    }

    // Method Description:
    // - Handler for clicking the GoForward button. This change the value of _goForward,
    //   mark GoForward button as checked and ensure GoBackward button
    //   is not checked
    // Arguments:
    // - sender: not used
    // - e: not used
    // Return Value:
    // - <none>
    void SearchBoxControl::GoForwardClicked(const winrt::Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& /*e*/)
    {
        GoForwardButton().IsChecked(true);
        if (GoBackwardButton().IsChecked())
        {
            GoBackwardButton().IsChecked(false);
        }

        // kick off search
        _SearchHandlers(TextBox().Text(), _GoForward(), _CaseSensitive());
    }

    // Method Description:
    // - Handler for clicking the close button. This destructs the
    //   search box object in TermControl
    // Arguments:
    // - sender: not used
    // - e: event data
    // Return Value:
    // - <none>
    void SearchBoxControl::CloseClick(const winrt::Windows::Foundation::IInspectable& /*sender*/, const RoutedEventArgs& e)
    {
        _ClosedHandlers(*this, e);
    }

    // Method Description:
    // - To avoid Characters input bubbling up to terminal, we implement this handler here,
    //   simply mark the key input as handled
    // Arguments:
    // - sender: not used
    // - e: event data
    // Return Value:
    // - <none>
    void SearchBoxControl::_CharacterHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/, const Input::CharacterReceivedRoutedEventArgs& e)
    {
        e.Handled(true);
    }

    // Method Description:
    // - Handler for changing the text. Triggers SearchChanged event
    // Arguments:
    // - sender: not used
    // - e: event data
    // Return Value:
    // - <none>
    void SearchBoxControl::TextBoxTextChanged(winrt::Windows::Foundation::IInspectable const& /*sender*/, winrt::Windows::UI::Xaml::RoutedEventArgs const& /*e*/)
    {
        _SearchChangedHandlers(TextBox().Text(), _GoForward(), _CaseSensitive());
    }

    // Method Description:
    // - Handler for clicking the case sensitivity toggle. Triggers SearchChanged event
    // Arguments:
    // - sender: not used
    // - e: not used
    // Return Value:
    // - <none>
    void SearchBoxControl::CaseSensitivityButtonClicked(winrt::Windows::Foundation::IInspectable const& /*sender*/, winrt::Windows::UI::Xaml::RoutedEventArgs const& /*e*/)
    {
        _SearchChangedHandlers(TextBox().Text(), _GoForward(), _CaseSensitive());
    }

    // Method Description:
    // - Formats a status message representing the search state:
    // * "Searching" - if totalMatches is negative
    // * "No results" - if totalMatches is 0
    // * "?/n" - if totalMatches=n matches and we didn't start the iteration over matches
    // (usually we will get this after buffer update)
    // * "m/n" - if we are currently at match m out of n.
    // * "m/max+" - if n > max results to show
    // * "?/max+" - if m > max results to show
    // Arguments:
    // - totalMatches - total number of matches (search results)
    // - currentMatch - the index of the current match (0-based)
    // Return Value:
    // - status message
    winrt::hstring SearchBoxControl::_FormatStatus(int32_t totalMatches, int32_t currentMatch)
    {
        if (totalMatches < 0)
        {
            return RS_(L"TermControl_Searching");
        }

        if (totalMatches == 0)
        {
            return RS_(L"TermControl_NoMatch");
        }

        std::wstring currentString;
        std::wstring totalString;

        if (currentMatch < 0 || currentMatch > (MaximumTotalResultsToShowInStatus - 1))
        {
            currentString = CurrentIndexTooHighStatus;
        }
        else
        {
            currentString = fmt::format(L"{}", currentMatch + 1);
        }

        if (totalMatches > MaximumTotalResultsToShowInStatus)
        {
            totalString = TotalResultsTooHighStatus;
        }
        else
        {
            totalString = fmt::format(L"{}", totalMatches);
        }

        return winrt::hstring{ fmt::format(RS_(L"TermControl_NumResults").c_str(), currentString, totalString) };
    }

    // Method Description:
    // - Helper method to measure the width of the text block given the text and the font size
    // Arguments:
    // - text: the text to measure
    // - fontSize: the size of the font to measure
    // Return Value:
    // - the size in pixels
    double SearchBoxControl::_TextWidth(winrt::hstring text, double fontSize)
    {
        winrt::Windows::UI::Xaml::Controls::TextBlock t;
        t.FontSize(fontSize);
        t.Text(text);
        t.Measure({ FLT_MAX, FLT_MAX });
        return t.ActualWidth();
    }

    // Method Description:
    // - This method tries to predict the maximal size of the status box
    // by measuring different possible statuses
    // Return Value:
    // - the size in pixels
    double SearchBoxControl::_GetStatusMaxWidth()
    {
        const auto fontSize = StatusBox().FontSize();
        const auto maxLength = std::max({ _TextWidth(_FormatStatus(-1, -1), fontSize),
                                          _TextWidth(_FormatStatus(0, -1), fontSize),
                                          _TextWidth(_FormatStatus(MaximumTotalResultsToShowInStatus, MaximumTotalResultsToShowInStatus - 1), fontSize),
                                          _TextWidth(_FormatStatus(MaximumTotalResultsToShowInStatus + 1, MaximumTotalResultsToShowInStatus - 1), fontSize),
                                          _TextWidth(_FormatStatus(MaximumTotalResultsToShowInStatus + 1, MaximumTotalResultsToShowInStatus), fontSize) });

        return maxLength;
    }

    // Method Description:
    // - Formats and sets the status message in the status box.
    // Increases the size of the box if required.
    // Arguments:
    // - totalMatches - total number of matches (search results)
    // - currentMatch - the index of the current match (0-based)
    // Return Value:
    // - <none>
    void SearchBoxControl::SetStatus(int32_t totalMatches, int32_t currentMatch)
    {
        const auto status = _FormatStatus(totalMatches, currentMatch);
        StatusBox().Text(status);
    }

    // Method Description:
    // - Enables / disables results navigation buttons
    // Arguments:
    // - enable: if true, the buttons should be enabled
    // Return Value:
    // - <none>
    void SearchBoxControl::NavigationEnabled(bool enabled)
    {
        GoBackwardButton().IsEnabled(enabled);
        GoForwardButton().IsEnabled(enabled);
    }
    bool SearchBoxControl::NavigationEnabled()
    {
        return GoBackwardButton().IsEnabled() || GoForwardButton().IsEnabled();
    }
}
