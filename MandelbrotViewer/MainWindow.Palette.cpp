#include "pch.h"
#include "MainWindow.xaml.h"
#include <sstream>

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::MandelbrotViewer::implementation
{
    void MainWindow::UpdateGradientPreview()
    {
        if (!m_previewBitmap) return;

        auto buffer = m_previewBitmap.PixelBuffer();
        auto byteAccess = buffer.as<::Windows::Storage::Streams::IBufferByteAccess>();

        uint8_t* pRaw = nullptr;
        if (SUCCEEDED(byteAccess->Buffer(&pRaw)) && pRaw)
        {
            uint32_t* pPixels = reinterpret_cast<uint32_t*>(pRaw);
            m_theme.GeneratePreview(pPixels, 256);
            m_previewBitmap.Invalidate();
        }
    }

    void MainWindow::OnThemePresetSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;

        const int index = ThemePresetComboBox().SelectedIndex();
        const auto& presets = Mandelbrot::GetAllPresets();
        if (index >= 0 && index < static_cast<int>(presets.size()))
        {
            m_theme.SetPreset(presets[index]);
            UpdateGradientPreview();
            PopulateStopSelector();
            RecolorBuffer();
        }
    }

    void MainWindow::OnFrequencySliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (IsSuppressingEvents()) return;
        m_frequency = static_cast<float>(e.NewValue());
        if (FrequencyValueText())
        {
            const int decimals = (m_frequency < 0.01f) ? 4 : ((m_frequency < 0.1f) ? 3 : 2);
            FrequencyValueText().Text(FormatFloat(m_frequency, decimals) + L"x");
        }

        RecolorBuffer();
    }

    void MainWindow::OnPhaseSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (IsSuppressingEvents()) return;
        m_phaseOffset = static_cast<float>(e.NewValue());
        if (PhaseValueText())
        {
            PhaseValueText().Text(FormatFloat(m_phaseOffset, 2));
        }

        RecolorBuffer();
    }

    void MainWindow::OnAnimateToggleClick(IInspectable const&, RoutedEventArgs const&)
    {
        if (AnimateToggle().IsChecked().GetBoolean())
        {
            m_animateTimer.Start();
        }
        else
        {
            m_animateTimer.Stop();
        }
    }

    void MainWindow::OnInvertChecked(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        m_invertColors = true;
        RecolorBuffer();
    }

    void MainWindow::OnInvertUnchecked(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        m_invertColors = false;
        RecolorBuffer();
    }

    void MainWindow::OnHistogramToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        m_histogramEqualize = HistogramToggle().IsOn();
        RecolorBuffer();
    }

    void MainWindow::OnLightingToggleChanged(IInspectable const&, RoutedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        m_lighting.enabled = LightingToggle().IsOn();
        if (LightingSettingsPanel())
        {
            LightingSettingsPanel().Visibility(m_lighting.enabled ? Visibility::Visible : Visibility::Collapsed);
        }
        RecolorBuffer();
    }

    void MainWindow::OnLightAzimuthSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (IsSuppressingEvents()) return;
        m_lighting.azimuthDeg = static_cast<float>(e.NewValue());
        if (LightAzimuthValueText())
        {
            LightAzimuthValueText().Text(winrt::to_hstring(static_cast<int>(std::lround(m_lighting.azimuthDeg))) + L"\u00B0");
        }
        if (m_lighting.enabled) RecolorBuffer();
    }

    void MainWindow::OnLightElevationSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (IsSuppressingEvents()) return;
        m_lighting.elevationDeg = static_cast<float>(e.NewValue());
        if (LightElevationValueText())
        {
            LightElevationValueText().Text(winrt::to_hstring(static_cast<int>(std::lround(m_lighting.elevationDeg))) + L"\u00B0");
        }
        if (m_lighting.enabled) RecolorBuffer();
    }

    void MainWindow::OnLightStrengthSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (IsSuppressingEvents()) return;
        m_lighting.strength = static_cast<float>(e.NewValue());
        if (LightStrengthValueText())
        {
            LightStrengthValueText().Text(FormatFloat(m_lighting.strength, 2));
        }
        if (m_lighting.enabled) RecolorBuffer();
    }

    void MainWindow::PopulateStopSelector()
    {
        {
            ScopedEventSuppress suppress(m_suppressEventDepth);

            StopSelectorComboBox().Items().Clear();
            const auto& stops = m_theme.Stops();
            for (size_t i = 0; i < stops.size(); ++i)
            {
                std::wostringstream ss;
                ss << L"Stop " << (i + 1) << L" (" << static_cast<int>(stops[i].position * 100.0f) << L"%)";
                StopSelectorComboBox().Items().Append(box_value(ss.str()));
            }

            if (!stops.empty())
            {
                StopSelectorComboBox().SelectedIndex(0);
            }
        }

        UpdateStopControlsFromSelected();
    }

    void MainWindow::UpdateStopControlsFromSelected()
    {
        const int index = StopSelectorComboBox().SelectedIndex();
        const auto& stops = m_theme.Stops();
        if (index < 0 || index >= static_cast<int>(stops.size())) return;

        const auto& stop = stops[index];

        ScopedEventSuppress suppress(m_suppressEventDepth);
        StopPositionSlider().Value(stop.position);
        StopPositionText().Text(FormatFloat(stop.position, 2));

        StopRedSlider().Value(stop.r);
        StopRedText().Text(winrt::to_hstring(stop.r));

        StopGreenSlider().Value(stop.g);
        StopGreenText().Text(winrt::to_hstring(stop.g));

        StopBlueSlider().Value(stop.b);
        StopBlueText().Text(winrt::to_hstring(stop.b));

        // Update swatch preview rectangle
        winrt::Windows::UI::Color color;
        color.A = 255;
        color.R = stop.r;
        color.G = stop.g;
        color.B = stop.b;
        StopColorSwatch().Fill(Media::SolidColorBrush(color));
    }

    void MainWindow::OnStopSelectionChanged(IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        UpdateStopControlsFromSelected();
    }

    void MainWindow::OnAddStopClick(IInspectable const&, RoutedEventArgs const&)
    {
        // Add a warm gold accent stop at the midpoint.
        m_theme.AddStop(0.5f, 255, 180, 0);
        UpdateGradientPreview();
        PopulateStopSelector();
        StopSelectorComboBox().SelectedIndex(static_cast<int>(m_theme.Stops().size() - 1));
        RecolorBuffer();
    }

    void MainWindow::OnRemoveStopClick(IInspectable const&, RoutedEventArgs const&)
    {
        const int index = StopSelectorComboBox().SelectedIndex();
        if (index >= 0 && m_theme.Stops().size() > 2)
        {
            m_theme.RemoveStop(index);
            UpdateGradientPreview();
            PopulateStopSelector();
            RecolorBuffer();
        }
    }

    void MainWindow::OnStopPositionSliderChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const& e)
    {
        if (IsSuppressingEvents()) return;
        const float pos = static_cast<float>(e.NewValue());
        if (StopPositionText())
        {
            StopPositionText().Text(FormatFloat(pos, 2));
        }

        const int index = StopSelectorComboBox().SelectedIndex();
        if (index >= 0 && index < static_cast<int>(m_theme.Stops().size()))
        {
            auto stop = m_theme.Stops()[index];
            m_theme.UpdateStop(index, pos, stop.r, stop.g, stop.b);
            {
                ScopedEventSuppress suppress(m_suppressEventDepth);
                std::wostringstream ss;
                ss << L"Stop " << (index + 1) << L" (" << static_cast<int>(pos * 100.0f) << L"%)";
                StopSelectorComboBox().Items().SetAt(static_cast<uint32_t>(index), box_value(ss.str()));
                StopSelectorComboBox().SelectedIndex(index);
            }
            UpdateGradientPreview();
            RecolorBuffer();
        }
    }

    void MainWindow::OnStopColorChanged(IInspectable const&, Primitives::RangeBaseValueChangedEventArgs const&)
    {
        if (IsSuppressingEvents()) return;
        const uint8_t r = static_cast<uint8_t>(StopRedSlider().Value());
        const uint8_t g = static_cast<uint8_t>(StopGreenSlider().Value());
        const uint8_t b = static_cast<uint8_t>(StopBlueSlider().Value());

        StopRedText().Text(winrt::to_hstring(r));
        StopGreenText().Text(winrt::to_hstring(g));
        StopBlueText().Text(winrt::to_hstring(b));

        winrt::Windows::UI::Color color;
        color.A = 255;
        color.R = r;
        color.G = g;
        color.B = b;
        StopColorSwatch().Fill(Media::SolidColorBrush(color));

        const int index = StopSelectorComboBox().SelectedIndex();
        if (index >= 0 && index < static_cast<int>(m_theme.Stops().size()))
        {
            const float pos = m_theme.Stops()[index].position;
            m_theme.UpdateStop(index, pos, r, g, b);
            UpdateGradientPreview();
            RecolorBuffer();
        }
    }
}
