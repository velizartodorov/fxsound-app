/*
FxSound
Copyright (C) 2025  FxSound LLC

Contributors:
    www.theremino.com (2025)
	
This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <JuceHeader.h>

//==============================================================================
/*
*/
class FxEqualizer : public Component, Slider::Listener, Timer
{
public:
    static FxEqualizer& getInstance()
    {
        static FxEqualizer equalizer;
        return equalizer;
    }

	FxEqualizer();
	~FxEqualizer() = default;

    void reinit(int num_bands);

    void sliderValueChanged(Slider*) override;
    void sliderDragStarted(Slider* slider) override;
    void sliderDragEnded(Slider* slider) override;

    void timerCallback() override;

	void update();
	void showValues(bool show);

private:
    class FxEqSlider : public Slider
    {
    public:
        FxEqSlider(int band, float max_gain);
        ~FxEqSlider() = default;

        void enablementChanged() override;

        void setGainValue(float value);
        void showValue(bool show);

    private:
        static constexpr int LABEL_HEIGHT = 11;

        void resized() override;
        void valueChanged() override;
        bool keyPressed(const KeyPress& key) override;
        void mouseDown(const juce::MouseEvent& event) override;

        Label gain_label_;
        int band_;
    };

    class FxBandCenterFreqSlider : public Slider
    {
    public:
        FxBandCenterFreqSlider(int band, Label& freq_label);
        ~FxBandCenterFreqSlider() = default;

        void setFrequency(float value);

        // The band's frequency range, on a logarithmic dial.
        void setFrequencyRange(float min_freq, float max_freq);

        void enablementChanged() override;

        void resetToDefaultFrequency();

    private:
        void valueChanged() override;
        bool keyPressed(const KeyPress& key) override;
        void mouseDown(const juce::MouseEvent& event) override;
        // Drags move in whole hertz. Values set from code (presets, the default frequency) keep their exact value.
        double snapValue(double attempted_value, DragMode drag_mode) override;
        // Moves 1 Hz up (direction > 0) or down (direction < 0), within the band's range. Used by the arrow keys.
        void stepFrequency(int direction);

        Label& freq_label_;

        int band_;
    };

    // The "ISO Mode" toggle: a small outlined label drawn in the power button's colours, muted.
    class FxIsoModeButton : public Button
    {
    public:
        FxIsoModeButton();
        ~FxIsoModeButton() = default;

    private:
        void paintButton(Graphics& g, bool highlighted, bool down) override;

        static constexpr float FONT_HEIGHT = 11.0f;
        static constexpr float CORNER_RADIUS = 3.0f;
    };

	static constexpr int WIDTH = 776;
	static constexpr int HEIGHT = 257;
	static constexpr int SLIDER_HEIGHT = 180;
	static constexpr int LABEL_HEIGHT = 11;
	static constexpr int SMALL_FONT = 10;
    static constexpr int ROTARY_SLIDER_HEIGHT = 36;
	static constexpr int X_MARGIN = 16;
	static constexpr int Y_MARGIN = 8;
    static constexpr int BUTTON_WIDTH = 18;
    static constexpr int BUTTON_HEIGHT = 18;
    static constexpr int BUTTON_MARGIN = 4;

	static constexpr float MAX_GAIN = 12.0f;

	void resized() override;
	void paint(Graphics& g) override;

    void restoreDefaults();
    void updateTooltips(int num_bands);
    // Re-reads every band's frequency range (it changes with the 10 band Standard / ISO layout) and frequency.
    void refreshBandFrequencies();
    // Shows the ISO toggle (10 bands only) in the controller's current layout.
    void updateIsoButton();

	std::vector<std::unique_ptr<Label>> labels_;
    std::vector<std::unique_ptr<FxEqSlider>> band_boosts_;
    std::vector<std::unique_ptr<FxBandCenterFreqSlider>> center_frequencies_;
    std::vector<float> band_gain_values_;

    DrawableButton restore_defaults_button_;
    std::unique_ptr<Drawable> restore_defaults_image_;
    std::unique_ptr<Drawable> restore_defaults_hover_image_;

    // 10 bands: switches between the Standard layout and the ISO layout (31, 62, 125 ... 16k).
    FxIsoModeButton iso_button_;
    // The layout the dials' ranges were last read for, so update() notices a change made by loading a preset.
    bool iso_mode_shown_ = false;
    // Sized and placed like the visible part of the Restore Defaults icon (its 16x16 artwork, scaled into the 18 px
    // button, draws from 1 px below the top to 2 px from the side): 15 px high, 1 px lower than the button row, and
    // 2 px further in from the edge.
    static constexpr int ISO_BUTTON_WIDTH = 60;
    static constexpr int ISO_BUTTON_HEIGHT = 15;
    static constexpr int ISO_BUTTON_TOP_INSET = 1;
    static constexpr int ISO_BUTTON_SIDE_INSET = 2;

    bool highlight_mode_;

    // State the tooltips were last built for, so paint() only rebuilds them when it changes
    bool tooltips_valid_ = false;
    bool tooltips_hidden_ = false;
    int tooltips_num_bands_ = 0;
    String tooltips_language_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxEqualizer)
};
