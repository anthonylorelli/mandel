#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"

#if __has_include("App.xaml.g.hpp")
#include "App.xaml.g.hpp"
#endif

namespace winrt::MandelbrotViewer::implementation
{
    App::App()
    {
        InitializeComponent();

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([this](winrt::Windows::Foundation::IInspectable const&, winrt::Microsoft::UI::Xaml::UnhandledExceptionEventArgs const& e)
        {
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(winrt::Microsoft::UI::Xaml::LaunchActivatedEventArgs const&)
    {
        window = winrt::make<MainWindow>();
        window.Activate();
    }
}

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    ::winrt::Microsoft::UI::Xaml::Application::Start([](auto&&)
    {
        ::winrt::make<::winrt::MandelbrotViewer::implementation::App>();
    });
    return 0;
}
