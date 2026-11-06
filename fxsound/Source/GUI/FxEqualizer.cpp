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

#include "FxEqualizer.h"
#include "FxController.h"
#include "FxTheme.h"

FxEqualizer::FxEqualizer() : restore_defaults_button_("eqRestoreDefaultsButton", DrawableButton::ButtonStyle::ImageFitted)
{
    auto& controller = FxController::getInstance();

    restore_defaults_image_ = Drawable::createFromImageData(FXIMAGE(RestoreDefaultsButton), FXIMAGESIZE(RestoreDefaultsButton));
    restore_defaults_hover_image_ = Drawable::createFromImageData(FXIMAGE(RestoreDefaultsButtonHover), FXIMAGESIZE(RestoreDefaultsButtonHover));
    restore_defaults_button_.setImages(restore_defaults_image_.get(), restore_defaults_hover_image_.get());
    restore_defaults_button_.setMouseCursor(MouseCursor::PointingHandCursor);
    restore_defaults_button_.setWantsKeyboardFocus(true);
    restore_defaults_button_.setTooltip(TRANS("Restore Defaults"));
    restore_defaults_button_.onClick = [this]() { restoreDefaults(); };

    iso_button_.onClick = [this]() {
        FxController::getInstance().setEqIsoMode(iso_button_.getToggleState());
        refreshBandFrequencies();
    };

    int num_bands = controller.getNumEqBands();
    labels_.resize(num_bands);
    band_boosts_.resize(num_bands);
    center_frequencies_.resize(num_bands);
    band_gain_values_.resize(num_bands);

    float min_freq, max_freq;
    for (int i = 0; i < num_bands; i++)
    {
        labels_[i].reset(new Label());
        band_boosts_[i].reset(new FxEqSlider(i, MAX_GAIN));
        center_frequencies_[i].reset(new FxBandCenterFreqSlider(i, *labels_[i]));
       
        labels_[i]->setJustificationType(Justification::centredTop);
        addAndMakeVisible(labels_[i].get());

        band_boosts_[i]->setSliderStyle(Slider::LinearVertical);
        band_boosts_[i]->setRange(-MAX_GAIN, MAX_GAIN, 1.0);
        band_boosts_[i]->setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
        band_boosts_[i]->addListener(this);
        addAndMakeVisible(band_boosts_[i].get());
       
        center_frequencies_[i]->setRotaryParameters(3.66519f, 8.90118f, true);
        FxController::getInstance().getEqBandFrequencyRange(i, &min_freq, &max_freq);
        center_frequencies_[i]->setFrequencyRange(min_freq, max_freq);
        center_frequencies_[i]->setFrequency(controller.getEqBandFrequency(i));
        addAndMakeVisible(center_frequencies_[i].get());

        band_gain_values_[i] = 0;
    }

    addAndMakeVisible(restore_defaults_button_);
    addChildComponent(iso_button_);
    updateIsoButton();

    highlight_mode_ = false;

    setSize(WIDTH, HEIGHT);
}

void FxEqualizer::reinit(int num_bands)
{   
    auto& controller = FxController::getInstance();

    removeAllChildren();

    tooltips_valid_ = false;

    // ------------------------------------------------------------ clear and reinitialize arrays
    labels_.clear();
    band_boosts_.clear();
    center_frequencies_.clear();
    band_gain_values_.clear();

    labels_.resize(num_bands);
    band_boosts_.resize(num_bands);
    center_frequencies_.resize(num_bands);
    band_gain_values_.resize(num_bands);

    // ------------------------------------------------------------ recreate all controls
    float min_freq, max_freq;

    for (int i = 0; i < num_bands; i++)
    {
        labels_[i].reset(new Label());
        band_boosts_[i].reset(new FxEqSlider(i, MAX_GAIN));
        center_frequencies_[i].reset(new FxBandCenterFreqSlider(i, *labels_[i]));
        
        labels_[i]->setJustificationType(Justification::centredTop);
        addAndMakeVisible(labels_[i].get());

        band_boosts_[i]->setSliderStyle(Slider::LinearVertical);
        band_boosts_[i]->setRange(-MAX_GAIN, MAX_GAIN, 1.0);
        band_boosts_[i]->setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
        band_boosts_[i]->addListener(this);
        addAndMakeVisible(band_boosts_[i].get());

        center_frequencies_[i]->setRotaryParameters(3.66519f, 8.90118f, true);
        controller.getEqBandFrequencyRange(i, &min_freq, &max_freq);
        center_frequencies_[i]->setFrequencyRange(min_freq, max_freq);
        center_frequencies_[i]->setFrequency(controller.getEqBandFrequency(i));
        addAndMakeVisible(center_frequencies_[i].get());

        band_gain_values_[i] = 0;
    }

    addAndMakeVisible(restore_defaults_button_);
    addChildComponent(iso_button_);
    updateIsoButton();

    resized();

    update();
    showValues(true);
}

void FxEqualizer::sliderValueChanged(Slider*)
{

}

void FxEqualizer::sliderDragStarted(Slider* slider)
{
    if (highlight_mode_)
    {
        return;
    }

    if (ModifierKeys::getCurrentModifiersRealtime().isAltDown())
    {
        auto& controller = FxController::getInstance();

        highlight_mode_ = true;

        startTimerHz(30);

        for (auto i = 0; i < band_boosts_.size(); i++)
        {
            if (band_boosts_[i].get() != slider)
            {
                band_boosts_[i]->setEnabled(false);
                band_boosts_[i]->showValue(false);

                band_gain_values_[i] = controller.getEqBandBoostCut(i);
            }
        }
    }
}

void FxEqualizer::sliderDragEnded(Slider* slider)
{
    if (highlight_mode_)
    {
        auto& controller = FxController::getInstance();
        highlight_mode_ = false;

        for (auto i = 0; i < band_boosts_.size(); i++) 
        {
            if (band_boosts_[i].get() != slider)
            {
                band_boosts_[i]->setEnabled(true);
                band_boosts_[i]->showValue(true);

                controller.setEqBandBoostCut(i, band_gain_values_[i]);
                band_boosts_[i]->setValue(band_gain_values_[i], NotificationType::dontSendNotification);
            }
        }
    }
}

void FxEqualizer::timerCallback()
{
    auto& controller = FxController::getInstance();

    if (highlight_mode_)
    {
        auto done = false;
        for (auto i = 0; i < band_boosts_.size(); i++)
        {
            if (!band_boosts_[i]->isEnabled())
            {
                auto gain = controller.getEqBandBoostCut(i);
                if (gain != -(MAX_GAIN-2))
                {
                    done = true;
                    if (gain < -(MAX_GAIN-2))
                    {
                        controller.setEqBandBoostCut(i, gain+1.0f);
                        band_boosts_[i]->setValue(gain+1.0f, NotificationType::dontSendNotification);
                    }
                    else
                    {
                        controller.setEqBandBoostCut(i, gain-1.0f);
                        band_boosts_[i]->setValue(gain-1.0f, NotificationType::dontSendNotification);
                    }
                }
            }            
        }

        if (!done)
        {
            stopTimer();
        }
    }
    else
    {
        stopTimer();
    }
}

FxEqualizer::FxIsoModeButton::FxIsoModeButton() : Button("IsoModeButton")
{
    setButtonText("ISO Mode");
    setClickingTogglesState(true);
    setMouseCursor(MouseCursor::PointingHandCursor);
    setWantsKeyboardFocus(true);
}

void FxEqualizer::FxIsoModeButton::paintButton(Graphics& g, bool highlighted, bool down)
{
    // On: the power-on colour (ImageButton: pink, or blue in the light theme). Off: the grey default text colour.
    // Muted text and border, a very light fill; slightly stronger while hovered or pressed. Colours are read here, so
    // a theme switch only needs a repaint.
    bool on = getToggleState();
    auto colour = on ? Colour(FXCOLOR(ImageButton)) : Colour(FXCOLOR(DefaultText));
    float emphasis = (highlighted || down) ? 0.15f : 0.0f;

    auto box = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(colour.withAlpha((on ? 0.08f : 0.05f) + emphasis * 0.5f));
    g.fillRoundedRectangle(box, CORNER_RADIUS);
    g.setColour(colour.withAlpha((on ? 0.5f : 0.3f) + emphasis));
    g.drawRoundedRectangle(box, CORNER_RADIUS, 1.0f);

    auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());
    g.setFont(theme.getNormalFont().withHeight(FONT_HEIGHT));
    g.setColour(colour.withAlpha((on ? 0.85f : 0.75f) + emphasis));
    g.drawText(getButtonText(), getLocalBounds(), Justification::centred, false);
}

void FxEqualizer::updateIsoButton()
{
    auto& controller = FxController::getInstance();

    iso_mode_shown_ = controller.isEqIsoMode();
    iso_button_.setToggleState(iso_mode_shown_, NotificationType::dontSendNotification);
    iso_button_.setVisible(controller.getNumEqBands() == 10);
}

void FxEqualizer::refreshBandFrequencies()
{
    auto& controller = FxController::getInstance();
    float min_freq, max_freq;

    for (auto i = 0; i < center_frequencies_.size() && i < controller.getNumEqBands(); i++)
    {
        controller.getEqBandFrequencyRange(i, &min_freq, &max_freq);
        center_frequencies_[i]->setFrequencyRange(min_freq, max_freq);
        center_frequencies_[i]->setFrequency(controller.getEqBandFrequency(i));
    }

    updateIsoButton();
}

void FxEqualizer::update()
{
    auto& controller = FxController::getInstance();

    // Loading a preset can switch the 10 band layout (Standard / ISO), which changes the dials' ranges.
    if (controller.isEqIsoMode() != iso_mode_shown_)
    {
        refreshBandFrequencies();
    }

    for (auto i = 0; i<band_boosts_.size() && i<controller.getNumEqBands(); i++)
    {
        auto value = controller.getEqBandBoostCut(i);
        if (value >= -MAX_GAIN && value <= MAX_GAIN)
        {
            band_boosts_[i]->setGainValue(value);
        }

        auto freq = controller.getEqBandFrequency(i);
        center_frequencies_[i]->setFrequency(freq);
    }
}

void FxEqualizer::restoreDefaults()
{
    auto& controller = FxController::getInstance();
    int num_bands = controller.getNumEqBands();

    for (auto i = 0; i < band_boosts_.size(); i++)
    {
        band_boosts_[i]->setValue(0.0, NotificationType::sendNotification);
    }

    // Center frequencies are only user-adjustable (and visible) below 15 bands,
    // matching the same threshold used for per-knob right/double-click reset.
    if (num_bands < 15)
    {
        for (auto i = 0; i < center_frequencies_.size(); i++)
        {
            center_frequencies_[i]->resetToDefaultFrequency();
        }
    }
}

void FxEqualizer::showValues(bool show)
{
    for (int i = 0; i < band_boosts_.size(); i++)
    {
        band_boosts_[i]->showValue(show);
    }
}

void FxEqualizer::resized()
{
    auto& controller = FxController::getInstance();

    int num_bands = controller.getNumEqBands();

    restore_defaults_button_.setBounds(getWidth() - BUTTON_WIDTH - BUTTON_MARGIN, BUTTON_MARGIN, BUTTON_WIDTH, BUTTON_HEIGHT);
    // Top-left corner, mirroring the visible Restore Defaults icon at the top right: same height, same distance from
    // the top and from its side of the page.
    iso_button_.setBounds(BUTTON_MARGIN + ISO_BUTTON_SIDE_INSET, restore_defaults_button_.getY() + ISO_BUTTON_TOP_INSET,
        ISO_BUTTON_WIDTH, ISO_BUTTON_HEIGHT);

    // With 10 bands the ISO Mode button is too wide for the margin beside the first slider, so the sliders start
    // below the button row and are shorter by the same amount.
    int sliders_top = (num_bands == 10) ? iso_button_.getBottom() + BUTTON_MARGIN : Y_MARGIN;
    int slider_height = SLIDER_HEIGHT - (sliders_top - Y_MARGIN);

    if (num_bands != labels_.size())
    {
		repaint();
        return;
    }

    auto bounds = getLocalBounds();

    int x = X_MARGIN;
    int width = (bounds.getWidth() - X_MARGIN*2) / static_cast<int>(labels_.size());
    int rotary_width = jmin(ROTARY_SLIDER_HEIGHT, width);

    bool show_center_frequencies = true;
    if (labels_.size() > 10)
    {
        show_center_frequencies = false;
    }

    auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());
    for (auto i=0; i<labels_.size(); i++)
    {
        center_frequencies_[i]->setVisible(show_center_frequencies);
        if (show_center_frequencies)
        {
            labels_[i]->setFont(theme.getNormalFont().withHeight(LABEL_HEIGHT));
            band_boosts_[i]->setBounds(x + (width - FxTheme::SLIDER_THUMB_RADIUS * 4) / 2, sliders_top, FxTheme::SLIDER_THUMB_RADIUS * 4, slider_height);
            labels_[i]->setBounds(x, band_boosts_[i]->getBottom() + 6, width, LABEL_HEIGHT);
            center_frequencies_[i]->setBounds(x + (width - rotary_width) / 2, labels_[i]->getBottom() + 4, rotary_width, rotary_width);
        }
        else
        {
            labels_[i]->setFont(theme.getNormalFont().withHeight(SMALL_FONT));
            band_boosts_[i]->setBounds(x + (width - FxTheme::SLIDER_THUMB_RADIUS * 4) / 2, Y_MARGIN, FxTheme::SLIDER_THUMB_RADIUS * 4, SLIDER_HEIGHT + ROTARY_SLIDER_HEIGHT);
            labels_[i]->setBounds(x, band_boosts_[i]->getBottom() + 6, width, LABEL_HEIGHT*2);
        }
               
        x += width;
    }
}

void FxEqualizer::updateTooltips(int num_bands)
{
    auto& controller = FxController::getInstance();

    auto hidden = controller.isHelpTooltipsHidden();
    auto language = controller.getLanguage();

    if (tooltips_valid_ && tooltips_hidden_ == hidden && tooltips_num_bands_ == num_bands && tooltips_language_ == language)
    {
        return;
    }

    tooltips_valid_ = true;
    tooltips_hidden_ = hidden;
    tooltips_num_bands_ = num_bands;
    tooltips_language_ = language;

    if (hidden)
    {
        for (int i = 0; i < num_bands; i++)
        {
            band_boosts_[i]->setTooltip("");
            center_frequencies_[i]->setTooltip("");
        }
        iso_button_.setTooltip("");
        return;
    }

    iso_button_.setTooltip(TRANS("ISO band layout: 31, 62, 125, 250, 500 Hz, 1, 2, 4, 8, 16 kHz. Loading a preset switches back to the standard layout, unless it was saved in ISO mode."));

    StringArray tool_tips = { TRANS("Hyper-low Bass - First band for very low frequencies down to 20 Hz."),
                              TRANS("Super-low Bass. Increase this for more rumble and \"thump\", decrease if there's too much boominess."),
                              TRANS("Center of your Bass sound. Increase this for a fuller low end, decrease if the bass sounds overwhelming."),
                              TRANS("The low end of your mid-range. Increase this to make vocals sound rich and warm, decrease it to help control instruments that sound loud and muffled."),
                              TRANS("A focal point of the low-mid-range. Increase this to bring out electric guitars and vocal volume, decrease it to reduce any \"boxy\" tones."),
                              TRANS("The center mid-range band. Increase this to drastically boost rhythm instruments and snare hits, reduce it to cut out \"nasal\" tones."),
                              TRANS("The high-mid-range. Increase this to get more instrumental harmonics, reduce it to improve drums that have too much \"clickiness\" or orchestral instruments that are piercing."),
                              TRANS("The lower end of the high-end range. Increase this for more vocal clarity and articulation, reduce it and move the frequency wheel up and down to find and cut out overly loud \"S\" and \"T\" sounds."),
                              TRANS("The core high-end range. Increase this to make your audio sound more like it's in an airy, large space, reduce it to help with room noises and unwanted echoing."),
                              TRANS("The highest range of average human hearing. Increase this to give your sound more of a crisp tone, with lots of overtones. Reduce it to remove hiss or painfully high sounds.") };

    String center_freq_tip = TRANS("This wheel allows you to adjust which frequencies this EQ band is affecting\r\n"
                                   "up or down to target different frequencies/pitches. The EQ slider above\r\n"
                                   "controls the volume of this EQ band. Increase or decrease to boost or cut\r\n" 
                                   "a portion of your audio's frequencies, without modifying the rest of your sound.");

    for (int i = 0; i < num_bands; i++)
    {
        if (num_bands == 10) band_boosts_[i]->setTooltip(tool_tips[i]);
        center_frequencies_[i]->setTooltip(center_freq_tip);
    }
}

void FxEqualizer::paint(Graphics& g)
{
    auto& controller = FxController::getInstance();

    int num_bands = controller.getNumEqBands();

    if (num_bands != labels_.size())
    {
		reinit(num_bands);
    }

    g.setFillType(FillType(Colour(FXCOLOR(ControlBackground)).withAlpha(1.0f)));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);

    Path path;

    Colour line_colour = Colour(FXCOLOR(SliderTrack)).withAlpha(1.0f);
    Colour gradient_colour_1 = Colour(FXCOLOR(EqStart)).withAlpha(0.34f);
    Colour gradient_colour_2 = Colour(FXCOLOR(EqEnd)).withAlpha(0.0f);

    if (!isEnabled())
    {
        line_colour = line_colour.withSaturation(0.0);
        gradient_colour_1 = gradient_colour_1.withSaturation(0.0);
        gradient_colour_2 = gradient_colour_2.withSaturation(0.0);
    }
    else
    {
        updateTooltips(num_bands);
    }

    if (highlight_mode_)
    {
        gradient_colour_2 = gradient_colour_2.withSaturation(0.0);
    }

    // One stroke per colour instead of one per segment, so only two edge tables are built
    Path enabled_lines;
    Path disabled_lines;

    for (auto i = 0; i < band_boosts_.size() - 1; i++)
    {
        auto y0 = static_cast<float>(band_boosts_[i]->getPositionOfValue(band_boosts_[i]->getValue()) + band_boosts_[i]->getY());
        auto y1 = static_cast<float>(band_boosts_[i + 1]->getPositionOfValue(band_boosts_[i + 1]->getValue()) + band_boosts_[i + 1]->getY());

        auto x0 = static_cast<float>(band_boosts_[i]->getX() + band_boosts_[i]->getWidth() / 2);
        auto x1 = static_cast<float>(band_boosts_[i + 1]->getX() + band_boosts_[i + 1]->getWidth() / 2);

        auto& lines = (band_boosts_[i]->isEnabled() || band_boosts_[i + 1]->isEnabled()) ? enabled_lines : disabled_lines;
        lines.addLineSegment(Line<float>(x0, y0, x1, y1), 1.0f);
    }

    if (!enabled_lines.isEmpty())
    {
        g.setColour(line_colour);
        g.strokePath(enabled_lines, PathStrokeType(1.0));
    }

    if (!disabled_lines.isEmpty())
    {
        g.setColour(line_colour.withSaturation(0.0));
        g.strokePath(disabled_lines, PathStrokeType(1.0));
    }

    for (auto i = 0; i < band_boosts_.size(); i++) 
    {
        auto x = static_cast<float>(band_boosts_[i]->getX() + band_boosts_[i]->getWidth() / 2);
        auto y = static_cast<float>(band_boosts_[i]->getPositionOfValue(band_boosts_[i]->getValue()) + band_boosts_[i]->getY());

        if (i == 0)                                           
        {
            path.startNewSubPath(Point<float>(x, static_cast<float>(band_boosts_[i]->getBottom() - FxTheme::SLIDER_THUMB_RADIUS)));
        }

        path.lineTo(x, y);

        if (i == band_boosts_.size() - 1)
        {
            path.lineTo(x, static_cast<float>(band_boosts_[i]->getBottom() - FxTheme::SLIDER_THUMB_RADIUS));
        }
    }
    path.closeSubPath();

    auto gradient = ColourGradient(gradient_colour_1, 0, static_cast<float>(band_boosts_[1]->getY()),
                                   gradient_colour_2, 0, static_cast<float>(band_boosts_[1]->getBottom()), false);
    g.setFillType(FillType(gradient));
    g.fillPath(path);
}

FxEqualizer::FxEqSlider::FxEqSlider(int band, float)
{
    band_ = band;

    setMouseCursor(MouseCursor::PointingHandCursor);

    auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

    gain_label_.setFont(theme.getSmallFont().withHeight(LABEL_HEIGHT));
    gain_label_.setJustificationType(Justification::centred);
    gain_label_.setInterceptsMouseClicks(false, false);
    addChildComponent(gain_label_);

    setWantsKeyboardFocus(true);
}

void FxEqualizer::FxEqSlider::setGainValue(float value)
{
    setValue(value, NotificationType::dontSendNotification);

    auto text = String::formatted(value == 0.0 ? "%.0f" : "%+.0f", value);
    gain_label_.setText(text, NotificationType::dontSendNotification);

    auto y = getPositionOfValue(value) - (FxTheme::SLIDER_THUMB_RADIUS *3);
    gain_label_.setBounds(gain_label_.getBounds().withY(static_cast<int>(y)));
}

void FxEqualizer::FxEqSlider::showValue(bool show)
{
    gain_label_.setVisible(show && isEnabled());
}

void FxEqualizer::FxEqSlider::enablementChanged()
{
    if (isEnabled())
    {
        setMouseCursor(MouseCursor::PointingHandCursor);
    }
    else
    {
        setMouseCursor(MouseCursor::NormalCursor);
    }
}

void FxEqualizer::FxEqSlider::resized()
{
    Slider::resized();
    gain_label_.setBounds(gain_label_.getX(), gain_label_.getY(), getWidth(), LABEL_HEIGHT);
}

void FxEqualizer::FxEqSlider::valueChanged()
{
    auto value = getValue();
    if (value != FxController::getInstance().getEqBandBoostCut(band_))
    {
        FxController::getInstance().setEqBandBoostCut(band_, static_cast<float>(value));

        auto text = String::formatted(value == 0.0 ? "%.0f" : "%+.0f", value);
        gain_label_.setText(text, NotificationType::dontSendNotification);

        auto y = getPositionOfValue(value) - (FxTheme::SLIDER_THUMB_RADIUS*3);
        gain_label_.setBounds(gain_label_.getBounds().withY(static_cast<int>(y)));
    }
}

bool FxEqualizer::FxEqSlider::keyPressed(const KeyPress& key)
{
    if (isEnabled())
    {
        if (key.isKeyCode(KeyPress::upKey))
        {
            setValue(getValue() + getInterval());
            return true;
        }
        else if (key.isKeyCode(KeyPress::downKey))
        {
            setValue(getValue() - getInterval());
            return true;
        }
    }

    return false;
}

// -------------------------------------------------------------------------- SLIDERS GAIN - centered with right mouseDown
void FxEqualizer::FxEqSlider::mouseDown(const juce::MouseEvent& event)
{
    // Check if right mouse button is pressed, or this is a double-click
    if (event.mods.isRightButtonDown() || event.getNumberOfClicks() >= 2)
    {
        // Reset the slider to 0. See FxAudioSlider::mouseDown for why this is
        // handled here rather than in mouseDoubleClick.
        setValue(0.0, juce::NotificationType::sendNotification);
    }
    else
    {
        // Default behavior for other buttons
        juce::Slider::mouseDown(event);
    }
}

namespace
{
    // A frequency to at most max_decimals decimals, without trailing zeros: 1.25, 1.36, 16.
    String trimmedNumber(double value, int max_decimals)
    {
        auto text = String(value, max_decimals);
        if (text.containsChar('.'))
        {
            text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
        }
        return text;
    }

    // "31 Hz", "62 Hz", "116 Hz", "1 kHz", "1.36 kHz", "16 kHz"; separator goes between the number and the unit.
    // Hertz are whole numbers with halves rounded down, so the ISO layout's 31.25 and 62.5 read as the conventional
    // 31 and 62.
    String formatFrequency(double value, const String& separator)
    {
        if (value >= 1000.0)
        {
            return trimmedNumber(value / 1000.0, 2) + separator + "kHz";
        }
        return String(roundToInt(value - 0.001)) + separator + "Hz";
    }
}

FxEqualizer::FxBandCenterFreqSlider::FxBandCenterFreqSlider(int band, Label& freq_label) : Slider(Slider::SliderStyle::Rotary, Slider::TextEntryBoxPosition::NoTextBox), freq_label_(freq_label)
{
    band_ = band;

    setMouseCursor(MouseCursor::PointingHandCursor);

    setWantsKeyboardFocus(true);
}

void FxEqualizer::FxBandCenterFreqSlider::setFrequencyRange(float min_freq, float max_freq)
{
    // Linear, with a 1 Hz interval like the original dial, so dragging and Slider's own mouse wheel handling (which
    // moves by the wheel's share of the range, at least one interval) feel as before. The legal-value function only
    // clamps, so values set from code keep their exact frequency (e.g. 62.5) instead of snapping to a 1 Hz grid
    // anchored at the range start; user drags and wheel moves are rounded to whole hertz in snapValue.
    double lo = min_freq;
    double hi = jmax((double)max_freq, lo + 1.0);
    NormalisableRange<double> range(lo, hi,
        [](double start, double end, double proportion) { return start + (end - start) * proportion; },
        [](double start, double end, double value) { return (value - start) / (end - start); },
        [](double start, double end, double value) { return jlimit(start, end, value); });
    range.interval = 1.0;
    setNormalisableRange(range);
}

double FxEqualizer::FxBandCenterFreqSlider::snapValue(double attempted_value, DragMode)
{
    return jlimit(getMinimum(), getMaximum(), std::round(attempted_value));
}

void FxEqualizer::FxBandCenterFreqSlider::stepFrequency(int direction)
{
    // From the nearest whole hertz, so a fractional value (e.g. 62.5) steps to 63 / 62.
    double current = getValue();
    double target = jlimit(getMinimum(), getMaximum(), std::round(current) + (direction > 0 ? 1.0 : -1.0));
    if (target != current)
    {
        setValue(target, NotificationType::sendNotificationSync);
    }
}

void FxEqualizer::FxBandCenterFreqSlider::setFrequency(float value)
{
    setValue(value, NotificationType::dontSendNotification);

    if (value > 0)
    {
        // Bands >= 15 have narrow columns: number and unit on two lines.
        bool two_lines = FxController::getInstance().getNumEqBands() >= 15;
        freq_label_.setText(formatFrequency(value, two_lines ? "\n" : " "), NotificationType::dontSendNotification);
    }
}

void FxEqualizer::FxBandCenterFreqSlider::enablementChanged()
{
    if (isEnabled())
    {
        setMouseCursor(MouseCursor::PointingHandCursor);
    }
    else
    {
        setMouseCursor(MouseCursor::NormalCursor);
    }
}

void FxEqualizer::FxBandCenterFreqSlider::valueChanged()
{
    auto freq = static_cast<float>(getValue());

    if (freq != FxController::getInstance().getEqBandFrequency(band_))
    {
        FxController::getInstance().setEqBandFrequency(band_, freq);

        setFrequency(freq);
    }
}

bool FxEqualizer::FxBandCenterFreqSlider::keyPressed(const KeyPress& key)
{
    if (isEnabled())
    {
        if (key.isKeyCode(KeyPress::upKey))
        {
            stepFrequency(1);
            return true;
        }
        else if (key.isKeyCode(KeyPress::downKey))
        {
            stepFrequency(-1);
            return true;
        }
    }

    return false;
}

// ------------------------------------------------------------------------------ SLIDERS FREQ - centered with right mouseDown
void FxEqualizer::FxBandCenterFreqSlider::mouseDown(const juce::MouseEvent& event)
{
    // ------------------------------------------------------- Check if bands are > 15 (with fixed frequency)
    int nBands = FxController::getInstance().getNumEqBands();
    if (nBands >= 15) return;

    // ------------------------------------------------------- Check if right mouse button is pressed, or this is a double-click
    if (event.mods.isRightButtonDown() || event.getNumberOfClicks() >= 2)
    {
        // See FxAudioSlider::mouseDown for why this is handled here rather than
        // in mouseDoubleClick.
        resetToDefaultFrequency();
    }
    else
    {
        // Default behavior
        juce::Slider::mouseDown(event);
    }
}

void FxEqualizer::FxBandCenterFreqSlider::resetToDefaultFrequency()
{
    // GraphicEqGetDefaultBandFreq (via getDefaultEqBandFrequency) covers every band
    // count the UI allows (5/10/15/20/31 - see FxController::setNumEqBands), so this
    // always succeeds in practice; if it ever doesn't, leave the value unchanged
    // rather than guess a frequency.
    float default_freq;
    if (FxController::getInstance().getDefaultEqBandFrequency(band_, &default_freq))
    {
        setValue(default_freq, juce::NotificationType::sendNotification);
    }
}
