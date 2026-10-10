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

#include <JuceHeader.h>
#include "FxAudioControls.h"
#include "FxController.h"

FxAudioControls::FxAudioControls() : flip_button_("flipButton", DrawableButton::ButtonStyle::ImageFitted)
{
	page_ = EffectsPage;

	setLookAndFeel();

	addAndMakeVisible(effects_);
	addChildComponent(equalizer_control_);
	addChildComponent(brickwall_control_);

	effects_.addMouseListener(this, true);
	equalizer_control_.addMouseListener(this, true);
	brickwall_control_.addMouseListener(this, true);

	flip_button_.setMouseCursor(MouseCursor::PointingHandCursor);
	flip_button_.setSize(BUTTON_WIDTH, BUTTON_HEIGHT);
	flip_button_.setWantsKeyboardFocus(true);
	flip_button_.onClick = [this]() {
		showPage((page_ + 1) % NumPages);
		};

	addAndMakeVisible(flip_button_);

	setSize(WIDTH, HEIGHT);
}

void FxAudioControls::showPage(int page)
{
	page_ = page;

	effects_.setVisible(page_ == EffectsPage);
	equalizer_control_.setVisible(page_ == EqualizerPage);
	brickwall_control_.setVisible(page_ == BrickwallPage);
}

void FxAudioControls::update()
{
	effects_.update();
	effects_.showValues(true);

	equalizer_control_.update();
	brickwall_control_.update();
}

void FxAudioControls::showValues(bool show)
{
	effects_.showValues(show);
}

void FxAudioControls::setLookAndFeel()
{
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

	flip_image_ = Drawable::createFromImageData(FXIMAGE(FlipButton), FXIMAGESIZE(FlipButton));
	flip_hover_image_ = Drawable::createFromImageData(FXIMAGE(FlipButtonHover), FXIMAGESIZE(FlipButtonHover));

	flip_button_.setImages(flip_image_.get(), flip_hover_image_.get());

	equalizer_control_.setLookAndFeel(theme);
	brickwall_control_.setLookAndFeel(theme);
}

void FxAudioControls::resized()
{
	effects_.setBounds(0, 0, WIDTH, HEIGHT);
	equalizer_control_.setBounds(0, 0, WIDTH, HEIGHT);
	brickwall_control_.setBounds(0, 0, WIDTH, HEIGHT);

	flip_button_.setBounds(getWidth() - BUTTON_WIDTH - 5, 5, BUTTON_WIDTH, BUTTON_HEIGHT);
}

void FxAudioControls::paint(Graphics& g)
{
	g.setFillType(FillType(Colour(FXCOLOR(ControlBackground)).withAlpha(1.0f)));
	g.fillRoundedRectangle(getLocalBounds().toFloat(), 8.0f);
}

FxEffects::FxEffects()
{
	StringArray texts = { TRANS("Clarity"), TRANS("Ambience"), TRANS("Surround Sound"), TRANS("Dynamic Boost"), TRANS("Bass Boost") };
    
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

	labels_.resize(EffectType::NumEffects);
	effects_.resize(EffectType::NumEffects);

	for (int i = EffectType::Fidelity; i < EffectType::NumEffects; i++)
	{
		labels_[i].reset(new Label(texts[i], texts[i]));
		labels_[i]->setFont(theme.getNormalFont().withHeight(12.0f));
		labels_[i]->setJustificationType(Justification::topLeft);
		auto border = labels_[i]->getBorderSize();
		border.setLeft(0);
		labels_[i]->setBorderSize(border);
		addAndMakeVisible(labels_[i].get());

		effects_[i].reset(new FxEffectSlider(static_cast<EffectType>(i)));
		effects_[i]->setSliderStyle(Slider::LinearHorizontal);
		effects_[i]->setRange(0, 10, 1.0);
		effects_[i]->setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
		addAndMakeVisible(effects_[i].get());
	}
}

void FxEffects::update()
{
	auto& controller = FxController::getInstance();

	for (int i = EffectType::Fidelity; i < EffectType::NumEffects; i++)
	{
		auto value = controller.getEffectValue(static_cast<EffectType>(i));
		if (value >= 0.0 && value <= 1.0)
		{
			effects_[i]->setEffectValue(value*10.0f);
		}
	}
}

void FxEffects::showValues(bool show)
{
	for (int i = EffectType::Fidelity; i < EffectType::NumEffects; i++)
	{
		effects_[i]->showValue(show);
	}
}

void FxEffects::resized()
{
	auto bounds = getLocalBounds();

	int y = Y_MARGIN;
	for (int i = EffectType::Fidelity; i < EffectType::NumEffects; i++)
	{
		labels_[i]->setBounds(X_MARGIN+FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
		effects_[i]->setBounds(X_MARGIN, labels_[i]->getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);
		y = effects_[i]->getBottom() + 10;
	}
}

void FxEffects::paint([[maybe_unused]] Graphics& g)
{
    StringArray texts = { TRANS("Clarity"), TRANS("Ambience"), TRANS("Surround Sound"), TRANS("Dynamic Boost"), TRANS("Bass Boost") };
    StringArray tool_tips = { TRANS("Enhances and elevates high end\r\nfidelity and presence"),
                              TRANS("Thickens and smooths audio\r\nwith controlled reverberation"),
                              TRANS("Widens the left-right balance\r\nfor expansive, wide sound"),
                              TRANS("Increases overall volume and balance\r\nwith responsive processing"),
                              TRANS("Boosts low end for full,\r\nimpactful response") };

    auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

    for (int i = EffectType::Fidelity; i < EffectType::NumEffects; i++)
    {
        labels_[i]->setFont(theme.getNormalFont().withHeight(12.0f));
        labels_[i]->setText(texts[i], NotificationType::dontSendNotification);
        if (!FxController::getInstance().isHelpTooltipsHidden())
        {
            effects_[i]->setTooltip(tool_tips[i]);
        }
        else
        {
            effects_[i]->setTooltip("");
        }
    }
}

FxEffects::FxEffectSlider::FxEffectSlider(EffectType effect)
{
	effect_ = effect;

	setMouseCursor(MouseCursor::PointingHandCursor);

	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

	value_label_.setFont(theme.getSmallFont().withHeight(11.0f));
	value_label_.setJustificationType(Justification::centredLeft);
	value_label_.setInterceptsMouseClicks(false, false);
	addChildComponent(value_label_);

	setWantsKeyboardFocus(true);
}

void FxEffects::FxEffectSlider::setEffectValue(float value)
{
	setValue(value, NotificationType::dontSendNotification);

	auto text = String::formatted("%.0f", value);
	value_label_.setText(text, NotificationType::dontSendNotification);

	auto pos = getPositionOfValue(value);
	auto x = static_cast<int>(pos) + FxTheme::SLIDER_THUMB_RADIUS + 1;
	value_label_.setBounds(value_label_.getBounds().withX(x));
}

void FxEffects::FxEffectSlider::showValue(bool show)
{
	value_label_.setVisible(show && isEnabled());
}

void FxEffects::FxEffectSlider::enablementChanged()
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

void FxEffects::FxEffectSlider::resized()
{
	Slider::resized();

	value_label_.setBounds(value_label_.getX(), (getHeight() - LABEL_HEIGHT) / 2, FxTheme::SLIDER_THUMB_RADIUS * 3, LABEL_HEIGHT);
}

void FxEffects::FxEffectSlider::valueChanged()
{
	auto value = static_cast<float>(getValue());

	if (value != FxController::getInstance().getEffectValue(effect_)*10.0)
	{
		FxController::getInstance().setEffectValue(effect_, value);
		
		auto text = String::formatted("%.0f", value);
		value_label_.setText(text, NotificationType::dontSendNotification);

		auto pos = getPositionOfValue(value);
		auto x = static_cast<int>(pos) + FxTheme::SLIDER_THUMB_RADIUS + 1;
		value_label_.setBounds(value_label_.getBounds().withX(x));
	}
}

bool FxEffects::FxEffectSlider::keyPressed(const KeyPress& key)
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

void FxEffects::FxEffectSlider::mouseDown(const juce::MouseEvent& event)
{
	// Reset on right-click or double-click, matching the other EQ/audio sliders.
	// See FxAudioSlider::mouseDown for why the reset is handled here rather than
	// in mouseDoubleClick.
	if (event.mods.isRightButtonDown() || event.getNumberOfClicks() >= 2)
	{
		setValue(0.0, NotificationType::sendNotification);
	}
	else
	{
		Slider::mouseDown(event);
	}
}

FxEqualizerControl::FxEqualizerControl() :
	master_gain_slider_("%0.0f dB", 0.0f),
	volume_leveling_slider_("%.1f dB", 0.0f),
	filter_q_slider_("%.1fx", 1.0f),
	balance_slider_(0.0f),
	restore_defaults_button_("restoreDefaultsButton", DrawableButton::ButtonStyle::ImageFitted)
{
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());
	auto& controller = FxController::getInstance();

	setLookAndFeel(theme);

	equalizer_.setMouseCursor(MouseCursor::PointingHandCursor);
	equalizer_.setWantsKeyboardFocus(true);
	equalizer_.onChange = [this]() {
		auto id = equalizer_.getSelectedId();

		auto num_eq_bands = FxController::DEFAULT_NUM_EQ_BANDS;
		for (auto bands : equalizer_bands_)
		{
			if (bands == id)
			{
				num_eq_bands = bands;
				break;
			}
		}

		FxController::getInstance().setNumEqBands(num_eq_bands);
	};

	equalizer_.clear(NotificationType::dontSendNotification);
	for (auto bands : equalizer_bands_)
	{
		equalizer_.addItem(String(bands) + TRANS(" Bands"), bands);
	}
	selectEqualizerBands();

	master_gain_title_.setFont(theme.getNormalFont().withHeight(12.0f));
	master_gain_title_.setJustificationType(Justification::topLeft);
	auto border = master_gain_title_.getBorderSize();
	border.setLeft(0);
	master_gain_title_.setBorderSize(border);

	master_gain_slider_.setSliderStyle(Slider::LinearHorizontal);
	master_gain_slider_.setRange(-20, 20, 2);
	master_gain_slider_.setValue(controller.getMasterGain());
	master_gain_slider_.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
	master_gain_slider_.onValueChange = [this]() {
		auto value = master_gain_slider_.getValue();
		auto& controller = FxController::getInstance();

		if (controller.getMasterGain() != value)
			controller.setMasterGain((float)value);
		};

	volume_leveling_title_.setFont(theme.getNormalFont().withHeight(12.0f));
	volume_leveling_title_.setJustificationType(Justification::topLeft);
	border = volume_leveling_title_.getBorderSize();
	border.setLeft(0);
	volume_leveling_title_.setBorderSize(border);

	volume_leveling_slider_.setSliderStyle(Slider::LinearHorizontal);
	volume_leveling_slider_.setRange(0, 4, 0.5);
	volume_leveling_slider_.setValue(controller.getVolumeLeveling());
	volume_leveling_slider_.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
	volume_leveling_slider_.onValueChange = [this]() {
		auto value = volume_leveling_slider_.getValue();
		auto& controller = FxController::getInstance();

		if (controller.getVolumeLeveling() != value)
			controller.setVolumeLeveling((float)value);
		};

	filter_q_title_.setFont(theme.getNormalFont().withHeight(12.0f));
	filter_q_title_.setJustificationType(Justification::topLeft);
	border = filter_q_title_.getBorderSize();
	border.setLeft(0);
	filter_q_title_.setBorderSize(border);

	filter_q_slider_.setSliderStyle(Slider::LinearHorizontal);
	filter_q_slider_.setRange(1, 3, 0.5);
	filter_q_slider_.setValue(controller.getFilterQ());
	filter_q_slider_.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
	filter_q_slider_.onValueChange = [this]() {
		auto value = filter_q_slider_.getValue();
		auto& controller = FxController::getInstance();

		if (controller.getFilterQ() != value)
			controller.setFilterQ((float)value);
		};

	balance_title_.setFont(theme.getNormalFont().withHeight(12.0f));
	balance_title_.setJustificationType(Justification::topLeft);
	border = balance_title_.getBorderSize();
	border.setLeft(0);
	balance_title_.setBorderSize(border);

	left_label_.setFont(theme.getSmallFont().withHeight(11.0f));
	left_label_.setJustificationType(Justification::centredLeft);
	border = left_label_.getBorderSize();
	border.setLeft(0);
	border.setTop(0);
	left_label_.setBorderSize(border);

	right_label_.setFont(theme.getSmallFont().withHeight(11.0f));
	right_label_.setJustificationType(Justification::centredRight);
    border = right_label_.getBorderSize();
    border.setRight(0);
    border.setTop(0);
    right_label_.setBorderSize(border);

	restore_defaults_button_.setMouseCursor(MouseCursor::PointingHandCursor);
	restore_defaults_button_.setWantsKeyboardFocus(true);
	restore_defaults_button_.onClick = [this]() {
		restoreDefaults();
		};

	setText();

	addAndMakeVisible(equalizer_);
	addAndMakeVisible(master_gain_title_);
	addAndMakeVisible(master_gain_slider_);
	addAndMakeVisible(volume_leveling_title_);
	addAndMakeVisible(volume_leveling_slider_);
	addAndMakeVisible(filter_q_title_);
	addAndMakeVisible(filter_q_slider_);
	addAndMakeVisible(balance_title_);
	addAndMakeVisible(balance_slider_);
	addAndMakeVisible(left_label_);
	addAndMakeVisible(right_label_);
	addAndMakeVisible(restore_defaults_button_);
}

FxEqualizerControl::~FxEqualizerControl()
{
	equalizer_.onChange = nullptr;
	master_gain_slider_.onValueChange = nullptr;
	volume_leveling_slider_.onValueChange = nullptr;
	filter_q_slider_.onValueChange = nullptr;
	restore_defaults_button_.onClick = nullptr;
}

void FxEqualizerControl::update()
{
	auto& controller = FxController::getInstance();

	selectEqualizerBands();

	master_gain_slider_.setValue(controller.getMasterGain(), NotificationType::dontSendNotification);
	volume_leveling_slider_.setValue(controller.getVolumeLeveling(), NotificationType::dontSendNotification);
	filter_q_slider_.setValue(controller.getFilterQ(), NotificationType::dontSendNotification);
	balance_slider_.setValue(controller.getBalance(), NotificationType::dontSendNotification);
}

void FxEqualizerControl::setLookAndFeel([[maybe_unused]]FxTheme& theme)
{
	restore_defaults_image_ = Drawable::createFromImageData(FXIMAGE(RestoreDefaultsButton), FXIMAGESIZE(RestoreDefaultsButton));
	restore_defaults_hover_image_ = Drawable::createFromImageData(FXIMAGE(RestoreDefaultsButtonHover), FXIMAGESIZE(RestoreDefaultsButtonHover));

	restore_defaults_button_.setImages(restore_defaults_image_.get(), restore_defaults_hover_image_.get());
}

void FxEqualizerControl::resized()
{
	auto width = getWidth() - (X_MARGIN * 2);

	int y = Y_MARGIN;

	equalizer_.setBounds(X_MARGIN, y, width, COMBOBOX_HEIGHT);

	y = equalizer_.getBottom() + ROW_GAP;
	master_gain_title_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
	master_gain_slider_.setBounds(X_MARGIN, master_gain_title_.getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);

	y = master_gain_slider_.getBottom() + ROW_GAP;
	volume_leveling_title_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
	volume_leveling_slider_.setBounds(X_MARGIN, volume_leveling_title_.getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);

	y = volume_leveling_slider_.getBottom() + ROW_GAP;
	filter_q_title_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
	filter_q_slider_.setBounds(X_MARGIN, filter_q_title_.getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);

	y = filter_q_slider_.getBottom() + ROW_GAP;
	balance_title_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
	balance_slider_.setBounds(X_MARGIN, balance_title_.getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);

	y = balance_slider_.getBottom();
	left_label_.setBounds(X_MARGIN, y, SLIDER_WIDTH / 2, LABEL_HEIGHT);
	right_label_.setBounds(left_label_.getRight(), y, SLIDER_WIDTH / 2 - (FxTheme::SLIDER_THUMB_RADIUS * 4), LABEL_HEIGHT);

	y = left_label_.getBottom() + ROW_GAP;
	restore_defaults_button_.setBounds(X_MARGIN, y, BUTTON_WIDTH, BUTTON_HEIGHT);
}

void FxEqualizerControl::paint([[maybe_unused]]Graphics& g)
{
	setText();
	updateEqualizerBandsText();
}

void FxEqualizerControl::setText()
{
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());
	auto font = theme.getNormalFont().withHeight(12.0f);

	master_gain_title_.setFont(font);
	master_gain_title_.setText(TRANS("Master Gain"), NotificationType::dontSendNotification);

	volume_leveling_title_.setFont(font);
	volume_leveling_title_.setText(TRANS("Volume Leveling"), NotificationType::dontSendNotification);

	filter_q_title_.setFont(font);
	filter_q_title_.setText(TRANS("Filter Q"), NotificationType::dontSendNotification);

	balance_title_.setFont(font);
	balance_title_.setText(TRANS("Balance"), NotificationType::dontSendNotification);

	left_label_.setFont(theme.getSmallFont().withHeight(11.0f));
	left_label_.setText(TRANS("Left"), NotificationType::dontSendNotification);

	right_label_.setFont(theme.getSmallFont().withHeight(11.0f));
	right_label_.setText(TRANS("Right"), NotificationType::dontSendNotification);

	restore_defaults_button_.setTooltip(TRANS("Restore Defaults"));
}

void FxEqualizerControl::updateEqualizerBandsText()
{
	auto id = equalizer_.getSelectedId();

	for (auto bands : equalizer_bands_)
	{
		equalizer_.changeItemText(bands, String(bands) + TRANS(" Bands"));
	}

	if (equalizer_.getSelectedId() == 0 && id != 0)
	{
		equalizer_.setSelectedId(id, juce::dontSendNotification);
	}
}

void FxEqualizerControl::selectEqualizerBands()
{
	auto& controller = FxController::getInstance();
	auto num_eq_bands = controller.getNumEqBands();

	switch (num_eq_bands)
	{
	case 5:
	case 10:
	case 15:
	case 20:
	case 31:
		equalizer_.setSelectedId(num_eq_bands, NotificationType::dontSendNotification);
		break;

	default:
		equalizer_.setSelectedId(FxController::DEFAULT_NUM_EQ_BANDS, NotificationType::dontSendNotification);
	}
}

void FxEqualizerControl::restoreDefaults()
{
	auto& controller = FxController::getInstance();

	controller.setNumEqBands(FxController::DEFAULT_NUM_EQ_BANDS);
	controller.setVolumeLeveling(FxController::DEFAULT_VOLUME_LEVELING);
	controller.setBalance(FxController::DEFAULT_BALANCE);
	controller.setFilterQ(FxController::DEFAULT_FILTER_Q);
	controller.setMasterGain(FxController::DEFAULT_MASTER_GAIN);

	equalizer_.setSelectedId(controller.getNumEqBands(), NotificationType::dontSendNotification);
	master_gain_slider_.setValue(controller.getMasterGain());
	volume_leveling_slider_.setValue(controller.getVolumeLeveling());
	filter_q_slider_.setValue(controller.getFilterQ());
	balance_slider_.setValue(controller.getBalance());
}

void FxEqualizerControl::visibilityChanged()
{
	if (isVisible())
	{
		update();
	}
}

namespace
{
	// The mode slider has 4 positions, whose meaning depends on the type
	// slider: DfxDsp::BrickwallSteepness (Zero Latency) or
	// DfxDsp::BrickwallLinearPhaseLatency (Linear Phase). Both enums are 0-3.
	// Text is built fresh on every call so it follows language changes.
	constexpr int BRICKWALL_TYPE_ZERO_LATENCY = 0;
	constexpr int BRICKWALL_TYPE_LINEAR_PHASE = 1;
	constexpr int BRICKWALL_NUM_MODES = 4;

	int brickwallLatencyMs(int position)
	{
		switch (position)
		{
		case DfxDsp::BrickwallLinearPhaseLatency::Low:
			return 20;
		case DfxDsp::BrickwallLinearPhaseLatency::Medium:
			return 160;
		case DfxDsp::BrickwallLinearPhaseLatency::High:
			return 320;
		case DfxDsp::BrickwallLinearPhaseLatency::Max:
		default:
			return 640;
		}
	}

	// Value shown next to the steepness slider's thumb: just the slope. The
	// preset's name and what the number means are in the tooltip.
	String brickwallSteepnessValue(int position)
	{
		switch (position)
		{
		case DfxDsp::BrickwallSteepness::Gentle:
			return "12 dB";
		case DfxDsp::BrickwallSteepness::Standard:
			return "48 dB";
		case DfxDsp::BrickwallSteepness::Steep:
			return "96 dB";
		case DfxDsp::BrickwallSteepness::UltraSteep:
		default:
			return "132 dB";
		}
	}

	String brickwallSteepnessTooltip(int position)
	{
		String meaning = TRANS("The dB value is the slope in dB per octave: how much the level drops for every halving of frequency below the cutoff.");

		switch (position)
		{
		case DfxDsp::BrickwallSteepness::Gentle:
			return TRANS("Gentle (12 dB/oct): gentlest rolloff and lowest CPU use, but lets more content below the cutoff through.") + "\n" + meaning;
		case DfxDsp::BrickwallSteepness::Standard:
			return TRANS("Standard (48 dB/oct): balanced rolloff and CPU use. Recommended for most listening.") + "\n" + meaning;
		case DfxDsp::BrickwallSteepness::Steep:
			return TRANS("Steep (96 dB/oct): steep rolloff, at the cost of more CPU use and more phase shift near the cutoff.") + "\n" + meaning;
		case DfxDsp::BrickwallSteepness::UltraSteep:
		default:
			return TRANS("Ultra Steep (132 dB/oct): steepest rolloff and closest to a true brickwall, at the cost of the most CPU use and the most phase shift near the cutoff.") + "\n" + meaning;
		}
	}

	// Value shown next to the latency slider's thumb: just the delay. The
	// mode's name and what the number means are in the tooltip.
	String brickwallLatencyValue(int position)
	{
		return String(brickwallLatencyMs(position)) + " ms";
	}

	String brickwallLatencyTooltip(int position)
	{
		String meaning = TRANS("ms is the delay Linear Phase adds to all audio. A longer delay makes the cut steeper.");

		switch (position)
		{
		case DfxDsp::BrickwallLinearPhaseLatency::Low:
			return TRANS("Low (20 ms): smallest delay, but a soft cut (about -6dB at 10Hz with a 20Hz cutoff) that also slightly reduces bass just above the cutoff (about -2dB at 40Hz).") + "\n" + meaning;
		case DfxDsp::BrickwallLinearPhaseLatency::Medium:
			return TRANS("Medium (160 ms): steep cut (about 80 dB/octave at a 20Hz cutoff).") + "\n" + meaning;
		case DfxDsp::BrickwallLinearPhaseLatency::High:
			return TRANS("High (320 ms): very steep cut (about 215 dB/octave at a 20Hz cutoff). Noticeable delay on all audio.") + "\n" + meaning;
		case DfxDsp::BrickwallLinearPhaseLatency::Max:
		default:
			return TRANS("Max (640 ms): a true brickwall cut (about 440 dB/octave at a 20Hz cutoff). For music only - video and games will be noticeably out of sync.") + "\n" + meaning;
		}
	}

	// Each filter type keeps its own mode on the controller; this returns the
	// one for the type that is currently selected.
	int brickwallModePosition(FxController& controller)
	{
		if (controller.isBrickwallFilterLinearPhaseOn())
		{
			return static_cast<int>(controller.getBrickwallFilterLinearPhaseLatency());
		}

		return static_cast<int>(controller.getBrickwallFilterSteepness());
	}

	// The page is too narrow to name the cutoff in the toggle's text, so it
	// goes in the tooltip (0Hz = no filtering at all).
	String brickwallToggleTooltip(int hp_cutoff_hz)
	{
		if (hp_cutoff_hz <= 0)
		{
			return TRANS("The cutoff is 0Hz, so audio passes through unfiltered.");
		}

		return TRANS("Removes content below") + " " + String(hp_cutoff_hz) + "Hz.";
	}
}

FxBrickwallControl::ValueSlider::ValueSlider()
{
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

	// Same font and justification as FxAudioSlider's value label.
	value_label_.setFont(theme.getNormalFont().withHeight(12.0f));
	value_label_.setJustificationType(Justification::centredLeft);
	value_label_.setInterceptsMouseClicks(false, false);
	// Added to the parent in parentHierarchyChanged(), not to this slider, so
	// it isn't clipped to the slider's bounds.
}

void FxBrickwallControl::ValueSlider::setValueText(const String& text)
{
	value_label_.setText(text, NotificationType::dontSendNotification);
	positionValueLabel();
}

void FxBrickwallControl::ValueSlider::resized()
{
	Slider::resized();
	positionValueLabel();
}

void FxBrickwallControl::ValueSlider::moved()
{
	positionValueLabel();
}

void FxBrickwallControl::ValueSlider::valueChanged()
{
	positionValueLabel();
}

void FxBrickwallControl::ValueSlider::parentHierarchyChanged()
{
	auto* parent = getParentComponent();

	if (parent != nullptr && value_label_.getParentComponent() != parent)
	{
		parent->addAndMakeVisible(value_label_);
		positionValueLabel();
	}
}

void FxBrickwallControl::ValueSlider::enablementChanged()
{
	Slider::enablementChanged();
	value_label_.setEnabled(isEnabled()); // dims with the slider, as a child label would
}

void FxBrickwallControl::ValueSlider::positionValueLabel()
{
	// Placed like FxAudioSlider::updateLabel(): always just right of the thumb,
	// in the parent's coordinates (see parentHierarchyChanged()).
	auto text_width = value_label_.getFont().getStringWidth(value_label_.getText()) +
		value_label_.getBorderSize().getLeftAndRight() + 2;
	auto pos = (int)getPositionOfValue(getValue());
	auto x = getX() + pos + FxTheme::SLIDER_THUMB_RADIUS / 2 + 1;

	value_label_.setBounds(x, getY() + (getHeight() - LABEL_HEIGHT) / 2, text_width, LABEL_HEIGHT);
}

FxBrickwallControl::FxBrickwallControl() :
	hp_cutoff_slider_("%0.0f Hz", (float)FxController::DEFAULT_BRICKWALL_HP_CUTOFF_HZ),
	restore_defaults_button_("restoreDefaultsButton", DrawableButton::ButtonStyle::ImageFitted)
{
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

	filter_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	filter_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	filter_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	filter_toggle_.setWantsKeyboardFocus(true);
	filter_toggle_.onClick = [this]() {
		bool on = filter_toggle_.getToggleState();

		FxController::getInstance().setBrickwallFilterOn(on);
		if (!on)
		{
			preview_button_.setToggleState(false, NotificationType::dontSendNotification);
		}
		updateEnabled();
		};

	for (auto* title : { &type_title_, &mode_title_, &hp_cutoff_title_ })
	{
		title->setFont(theme.getNormalFont().withHeight(14));
		title->setJustificationType(Justification::topLeft);
		auto border = title->getBorderSize();
		border.setLeft(0);
		title->setBorderSize(border);
	}

	for (auto* slider : std::initializer_list<Slider*>{ &type_slider_, &mode_slider_, &hp_cutoff_slider_ })
	{
		slider->setSliderStyle(Slider::LinearHorizontal);
		slider->setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
		slider->setMouseCursor(MouseCursor::PointingHandCursor);
		slider->setWantsKeyboardFocus(true);
	}

	type_slider_.setRange(BRICKWALL_TYPE_ZERO_LATENCY, BRICKWALL_TYPE_LINEAR_PHASE, 1);
	type_slider_.onValueChange = [this]() {
		auto& controller = FxController::getInstance();

		controller.setBrickwallFilterLinearPhaseOn(static_cast<int>(type_slider_.getValue()) == BRICKWALL_TYPE_LINEAR_PHASE);
		mode_slider_.setValue(static_cast<double>(brickwallModePosition(controller)), NotificationType::dontSendNotification);
		updateLabels();
		};

	mode_slider_.setRange(0, BRICKWALL_NUM_MODES - 1, 1);
	mode_slider_.onValueChange = [this]() {
		auto& controller = FxController::getInstance();
		int position = static_cast<int>(mode_slider_.getValue());

		if (controller.isBrickwallFilterLinearPhaseOn())
		{
			controller.setBrickwallFilterLinearPhaseLatency(static_cast<DfxDsp::BrickwallLinearPhaseLatency>(position));
		}
		else
		{
			controller.setBrickwallFilterSteepness(static_cast<DfxDsp::BrickwallSteepness>(position));
		}
		updateLabels();
		};

	hp_cutoff_slider_.setRange(HP_CUTOFF_MIN_HZ, HP_CUTOFF_MAX_HZ, 1);
	hp_cutoff_slider_.onValueChange = [this]() {
		auto& controller = FxController::getInstance();
		int cutoff_hz = static_cast<int>(hp_cutoff_slider_.getValue());

		// update() resyncs this slider with a notification (so FxAudioSlider
		// refreshes its value text), so skip the controller when nothing
		// changed - the same guard the EQ page's sliders use.
		if (controller.getBrickwallFilterHighPassCutoff() != cutoff_hz)
		{
			controller.setBrickwallFilterHighPassCutoff(cutoff_hz);
		}
		updateLabels();
		};

	warning_label_.setFont(theme.getNormalFont().withHeight(12.0f));
	warning_label_.setJustificationType(Justification::topLeft);
	auto border = warning_label_.getBorderSize();
	border.setLeft(0);
	border.setTop(0);
	warning_label_.setBorderSize(border);

	restore_defaults_button_.setMouseCursor(MouseCursor::PointingHandCursor);
	restore_defaults_button_.setWantsKeyboardFocus(true);
	restore_defaults_button_.onClick = [this]() {
		restoreDefaults();
		};

	preview_button_.setWantsKeyboardFocus(true);
	preview_button_.onClick = [this]() {
		FxController::getInstance().setBrickwallFilterPreviewOn(preview_button_.getToggleState());
		};

	setText();
	update();

	addAndMakeVisible(filter_toggle_);
	addAndMakeVisible(type_title_);
	addAndMakeVisible(type_slider_);
	addAndMakeVisible(mode_title_);
	addAndMakeVisible(mode_slider_);
	addAndMakeVisible(hp_cutoff_title_);
	addAndMakeVisible(hp_cutoff_slider_);
	addChildComponent(warning_icon_);
	addChildComponent(warning_label_);
	addAndMakeVisible(restore_defaults_button_);
	addAndMakeVisible(preview_button_);
}

FxBrickwallControl::~FxBrickwallControl()
{
	filter_toggle_.onClick = nullptr;
	type_slider_.onValueChange = nullptr;
	mode_slider_.onValueChange = nullptr;
	hp_cutoff_slider_.onValueChange = nullptr;
	restore_defaults_button_.onClick = nullptr;
	preview_button_.onClick = nullptr;
}

void FxBrickwallControl::update()
{
	auto& controller = FxController::getInstance();

	filter_toggle_.setToggleState(controller.isBrickwallFilterOn(), NotificationType::dontSendNotification);
	type_slider_.setValue(controller.isBrickwallFilterLinearPhaseOn() ? BRICKWALL_TYPE_LINEAR_PHASE : BRICKWALL_TYPE_ZERO_LATENCY, NotificationType::dontSendNotification);
	mode_slider_.setValue(static_cast<double>(brickwallModePosition(controller)), NotificationType::dontSendNotification);
	hp_cutoff_slider_.setValue(static_cast<double>(controller.getBrickwallFilterHighPassCutoff()), NotificationType::sendNotificationSync);
	preview_button_.setToggleState(controller.isBrickwallFilterPreviewOn(), NotificationType::dontSendNotification);

	updateLabels();
	updateEnabled();
}

void FxBrickwallControl::setLookAndFeel(FxTheme&)
{
	restore_defaults_image_ = Drawable::createFromImageData(FXIMAGE(RestoreDefaultsButton), FXIMAGESIZE(RestoreDefaultsButton));
	restore_defaults_hover_image_ = Drawable::createFromImageData(FXIMAGE(RestoreDefaultsButtonHover), FXIMAGESIZE(RestoreDefaultsButtonHover));

	restore_defaults_button_.setImages(restore_defaults_image_.get(), restore_defaults_hover_image_.get());
}

void FxBrickwallControl::updateLabels()
{
	bool linear_phase = static_cast<int>(type_slider_.getValue()) == BRICKWALL_TYPE_LINEAR_PHASE;
	int position = static_cast<int>(mode_slider_.getValue());
	int hp_cutoff_hz = static_cast<int>(hp_cutoff_slider_.getValue());
	String warning_tooltip = TRANS("Linear Phase delays all system audio, so video and games may go out of sync. CPU use also grows with each latency step.");

	// The filter type is named in the title above its slider; the mode and
	// cutoff sliders have a fixed title and show their value next to the thumb,
	// as on the EQ page, with the mode's name explained in its tooltip.
	type_title_.setText(linear_phase ? TRANS("Linear Phase") : TRANS("Zero Latency"), NotificationType::dontSendNotification);
	type_slider_.setTooltip(TRANS("Zero Latency adds no delay but shifts phase near the cutoff. Linear Phase has no phase shift but delays all audio."));

	mode_title_.setText(linear_phase ? TRANS("Latency") : TRANS("Steepness"), NotificationType::dontSendNotification);
	mode_slider_.setValueText(linear_phase ? brickwallLatencyValue(position) : brickwallSteepnessValue(position));
	mode_slider_.setTooltip(linear_phase ? brickwallLatencyTooltip(position) : brickwallSteepnessTooltip(position));

	// The cutoff's value is shown next to the slider's thumb (FxAudioSlider).
	hp_cutoff_title_.setText(TRANS("Cutoff Frequency"), NotificationType::dontSendNotification);
	hp_cutoff_slider_.setTooltip(TRANS("Frequencies below this are removed. Set to 0Hz to pass audio through unfiltered.") + "\n" +
		TRANS("At the cutoff itself, Zero Latency is -3dB and Linear Phase is -6dB (Low latency cuts more softly)."));
	filter_toggle_.setTooltip(brickwallToggleTooltip(hp_cutoff_hz));

	warning_label_.setText(TRANS("Adds") + " ~" + String(brickwallLatencyMs(position)) + "ms " + TRANS("latency and higher CPU load"), NotificationType::dontSendNotification);
	warning_label_.setTooltip(warning_tooltip);
	warning_icon_.setTooltip(warning_tooltip);
	warning_icon_.setVisible(linear_phase);
	warning_label_.setVisible(linear_phase);
}

void FxBrickwallControl::updateEnabled()
{
	bool on = filter_toggle_.getToggleState();

	for (auto* component : std::initializer_list<Component*>{ &type_title_, &type_slider_, &mode_title_, &mode_slider_,
		&hp_cutoff_title_, &hp_cutoff_slider_, &warning_label_, &restore_defaults_button_, &preview_button_ })
	{
		component->setEnabled(on);
	}
	warning_icon_.setAlpha(on ? 1.0f : 0.5f);
}

void FxBrickwallControl::restoreDefaults()
{
	auto& controller = FxController::getInstance();

	// Settings only: the filter's on/off state is left as it is.
	controller.setBrickwallFilterPreviewOn(false);
	controller.setBrickwallFilterLinearPhaseOn(false);
	controller.setBrickwallFilterSteepness(DfxDsp::BrickwallSteepness::Standard);
	controller.setBrickwallFilterLinearPhaseLatency(DfxDsp::BrickwallLinearPhaseLatency::Medium);
	controller.setBrickwallFilterHighPassCutoff(FxController::DEFAULT_BRICKWALL_HP_CUTOFF_HZ);

	update();
}

void FxBrickwallControl::resized()
{
	auto width = getWidth() - (X_MARGIN * 2);

	int y = Y_MARGIN;
	filter_toggle_.setBounds(X_MARGIN, y, width, TOGGLE_HEIGHT);

	y = filter_toggle_.getBottom() + ROW_GAP;
	type_title_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
	type_slider_.setBounds(X_MARGIN, type_title_.getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);

	y = type_slider_.getBottom() + ROW_GAP;
	mode_title_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
	mode_slider_.setBounds(X_MARGIN, mode_title_.getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);

	y = mode_slider_.getBottom() + ROW_GAP;
	hp_cutoff_title_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, SLIDER_WIDTH, LABEL_HEIGHT);
	hp_cutoff_slider_.setBounds(X_MARGIN, hp_cutoff_title_.getBottom() + 1, SLIDER_WIDTH, SLIDER_HEIGHT);

	y = hp_cutoff_slider_.getBottom() + ROW_GAP;
	warning_icon_.setBounds(X_MARGIN + FxTheme::SLIDER_THUMB_RADIUS, y, WARNING_ICON_SIZE, WARNING_ICON_SIZE);
	auto warning_x = warning_icon_.getRight() + WARNING_ICON_GAP;
	warning_label_.setBounds(warning_x, y, X_MARGIN + SLIDER_WIDTH - warning_x, LABEL_HEIGHT * 2);

	restore_defaults_button_.setBounds(X_MARGIN, BUTTONS_Y, BUTTON_WIDTH, BUTTON_HEIGHT);
	preview_button_.setBounds(restore_defaults_button_.getRight() + BUTTON_GAP, BUTTONS_Y, BUTTON_WIDTH, BUTTON_HEIGHT);
}

void FxBrickwallControl::paint(Graphics&)
{
	setText();
}

void FxBrickwallControl::setText()
{
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

	filter_toggle_.setButtonText(TRANS("Low Cut Filter"));
	for (auto* title : { &type_title_, &mode_title_, &hp_cutoff_title_ })
	{
		title->setFont(theme.getNormalFont().withHeight(14));
	}
	warning_label_.setFont(theme.getNormalFont().withHeight(12.0f));

	restore_defaults_button_.setTooltip(TRANS("Restore Defaults"));
	preview_button_.setTooltip(TRANS("Plays back only the content the filter is removing, so you can hear what it affects."));

	updateLabels();
}

void FxBrickwallControl::visibilityChanged()
{
	if (isVisible())
	{
		update();
	}
	else if (preview_button_.getToggleState())
	{
		// Don't keep playing "what's removed" once its button is out of sight.
		FxController::getInstance().setBrickwallFilterPreviewOn(false);
		preview_button_.setToggleState(false, NotificationType::dontSendNotification);
	}
}

void FxBrickwallControl::PreviewButton::paintButton(Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
	auto bounds = getLocalBounds().toFloat().reduced(2.0f);

	// Active (previewing) matches the slider's own active/filled colour; grey
	// otherwise, whether that's "off but clickable" or "disabled" (master
	// filter off) - a 0.3-alpha grey on this dark theme's background reads as
	// essentially invisible, so the disabled state only dims slightly, not
	// enough to disappear. Colour(uint32) alone is fully transparent (the raw
	// FXCOLOR value has no alpha byte set) - every branch below must call
	// withAlpha() explicitly, matching this file's existing Colour(FXCOLOR(x))
	// usages elsewhere (e.g. SettingsButton::paint()).
	Colour colour;
	if (getToggleState())
	{
		colour = Colour(FXCOLOR(SliderTrack)).withAlpha(1.0f);
	}
	else if (isEnabled())
	{
		colour = Colour(FXCOLOR(DefaultText)).withAlpha(1.0f);
	}
	else
	{
		colour = Colour(FXCOLOR(DefaultText)).withAlpha(0.5f);
	}

	if (isEnabled() && (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown))
	{
		colour = colour.brighter(0.3f);
	}

	// Ear icon ("listen to what's removed"): an outer ear contour plus the inner
	// fold, stroked as outlines (no bitmap/SVG asset - this codebase's images go
	// through Projucer's BinaryData step, which isn't available in this
	// environment). Built in a fixed 24x24 normalized box, then one shared
	// transform fits the icon into the button's actual bounds. JUCE arc angles
	// are clockwise from 12 o'clock.
	constexpr float ICON_BOX = 24.0f;
	constexpr float pi = MathConstants<float>::pi;

	// Outer contour: top of the ear (left to right over the top), down the back
	// of the ear, then the earlobe curling back under to the left.
	Path outer_path;
	outer_path.addCentredArc(12.5f, 8.5f, 6.5f, 6.5f, 0.0f, -pi * 0.5f, pi * 0.5f, true);
	outer_path.cubicTo(19.0f, 14.5f, 13.0f, 14.5f, 13.0f, 18.5f);
	outer_path.addCentredArc(9.5f, 18.5f, 3.5f, 3.5f, 0.0f, pi * 0.5f, pi * 1.5f, false);

	// Inner fold: a small arc over the top, then a hook curling into the canal.
	Path inner_path;
	inner_path.addCentredArc(12.5f, 8.5f, 2.5f, 2.5f, 0.0f, pi * 0.5f, -pi * 0.5f, true);
	inner_path.lineTo(10.0f, 9.5f);
	inner_path.addCentredArc(10.0f, 11.5f, 2.0f, 2.0f, 0.0f, 0.0f, pi, false);

	auto scale = jmin(bounds.getWidth(), bounds.getHeight()) / ICON_BOX;
	auto offset_x = bounds.getX() + (bounds.getWidth() - ICON_BOX * scale) * 0.5f;
	auto offset_y = bounds.getY() + (bounds.getHeight() - ICON_BOX * scale) * 0.5f;
	auto transform = AffineTransform::scale(scale).translated(offset_x, offset_y);

	outer_path.applyTransform(transform);
	inner_path.applyTransform(transform);

	PathStrokeType stroke(ICON_BOX * scale * 0.09f, PathStrokeType::curved, PathStrokeType::rounded);
	g.setColour(colour);
	g.strokePath(outer_path, stroke);
	g.strokePath(inner_path, stroke);
}

void FxBrickwallControl::WarningIcon::paint(Graphics& g)
{
	// Amber warning triangle with "!" (hand-drawn - no bitmap/SVG asset, since
	// Projucer's BinaryData step isn't available). Amber stands out on both
	// themes without reading as an error. Dimming when the filter is off is
	// done with setAlpha() by FxBrickwallControl::updateEnabled().
	constexpr uint32 WARNING_AMBER = 0xFFF5A623;

	auto bounds = getLocalBounds().toFloat().reduced(1.0f);
	auto size = jmin(bounds.getWidth(), bounds.getHeight());
	auto area = bounds.withSizeKeepingCentre(size, size);
	auto stroke = size * 0.11f;

	Path triangle;
	triangle.addTriangle(area.getCentreX(), area.getY() + stroke * 0.5f,
		area.getRight() - stroke * 0.5f, area.getBottom() - stroke * 0.5f,
		area.getX() + stroke * 0.5f, area.getBottom() - stroke * 0.5f);

	g.setColour(Colour(WARNING_AMBER));
	g.strokePath(triangle, PathStrokeType(stroke, PathStrokeType::curved, PathStrokeType::rounded));
	g.drawLine(area.getCentreX(), area.getY() + size * 0.38f, area.getCentreX(), area.getY() + size * 0.66f, stroke);
	g.fillEllipse(area.getCentreX() - stroke * 0.6f, area.getY() + size * 0.76f, stroke * 1.2f, stroke * 1.2f);
}
