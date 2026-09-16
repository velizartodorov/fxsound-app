/*
FxSound
Copyright (C) 2025  FxSound LLC

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
#include "FxSettingsDialog.h"
#include "../Utils/SysInfo/SysInfo.h"

//==============================================================================
FxSettingsDialog::FxSettingsDialog() : FxWindow("Settings"), tooltip_window_(this)
{
	setContent(&settings_content_);
	centreWithSize(getWidth(), getHeight());
	addToDesktop(0);
	toFront(true);
}

void FxSettingsDialog::closeButtonPressed()
{
	FxController::getInstance().setBrickwallFilterPreviewOn(false);

	exitModalState(0);
	removeFromDesktop();
}

void FxSettingsDialog::paint(Graphics& g)
{
	FxWindow::paint(g);

	g.setColour(Colour(FXCOLOR(Outline)).withAlpha(1.0f));
	g.drawLine((float)SEPARATOR_X, (float)title_bar_.getBottom(), (float)SEPARATOR_X, (float)getLocalBounds().getBottom());
}

void FxSettingsDialog::SettingsButton::paint(Graphics& g)
{
	auto bounds = getLocalBounds();

	if (getToggleState())
	{
		g.setColour(Colour(FXCOLOR(MenuHighlightBackground)).withAlpha(1.0f));
	}
	else
	{
		g.setColour(Colour(FXCOLOR(MenuBackground)).withAlpha(1.0f));
	}
	auto rect = juce::Rectangle<float>(0, 0, (float)bounds.getHeight(), (float)bounds.getHeight());
	g.fillRoundedRectangle(rect, (float)bounds.getHeight()/4);

	image_->drawWithin(g, rect.reduced(10, 10), RectanglePlacement::centred, 1.0f);

	if (getToggleState())
	{
		g.setColour(Colour(FXCOLOR(HighlightedText)).withAlpha(1.0f));
	}
	else
	{
		g.setColour(Colour(FXCOLOR(DefaultText)).withAlpha(1.0f));
	}
	auto w = bounds.getWidth() - bounds.getHeight() + 5;

	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());
	g.setFont(theme.getNormalFont());
	g.drawText(TRANS(getName()), juce::Rectangle<int>(bounds.getHeight()+5, 0, w, bounds.getHeight()), Justification::centredLeft);
}

void FxSettingsDialog::BrickwallPreviewButton::paintButton(Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
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

	// Headphones icon: a headband arc over two ear cups - the standard
	// "monitor/preview audio" symbol in audio software (no bitmap/SVG asset -
	// this codebase's images go through Projucer's BinaryData step, which
	// isn't available in this environment). Built in a fixed 24x24 normalized
	// box, then one shared transform fits the whole icon (headband + both
	// cups) into the button's actual bounds, so the pieces stay proportioned
	// to each other regardless of button size.
	constexpr float ICON_BOX = 24.0f;

	Path headband_path;
	headband_path.addCentredArc(12.0f, 13.0f, 8.5f, 8.5f, 0.0f,
		MathConstants<float>::pi * 1.5f, MathConstants<float>::pi * 2.5f, true);

	Path left_cup_path;
	left_cup_path.addRoundedRectangle(2.5f, 12.0f, 5.0f, 8.5f, 2.2f);

	Path right_cup_path;
	right_cup_path.addRoundedRectangle(16.5f, 12.0f, 5.0f, 8.5f, 2.2f);

	auto scale = jmin(bounds.getWidth(), bounds.getHeight()) / ICON_BOX;
	auto offset_x = bounds.getX() + (bounds.getWidth() - ICON_BOX * scale) * 0.5f;
	auto offset_y = bounds.getY() + (bounds.getHeight() - ICON_BOX * scale) * 0.5f;
	auto transform = AffineTransform::scale(scale).translated(offset_x, offset_y);

	headband_path.applyTransform(transform);
	left_cup_path.applyTransform(transform);
	right_cup_path.applyTransform(transform);

	g.setColour(colour);
	g.strokePath(headband_path, PathStrokeType(ICON_BOX * scale * 0.09f, PathStrokeType::curved, PathStrokeType::rounded));
	g.fillPath(left_cup_path);
	g.fillPath(right_cup_path);
}

bool FxSettingsDialog::keyPressed(const KeyPress& key)
{
	if (key == KeyPress::escapeKey)
	{
		exitModalState(0);
		removeFromDesktop();
		return true;
	}

	return Component::keyPressed(key);
}

FxSettingsDialog::SettingsComponent::SettingsComponent()
{
	audio_button_ = std::make_unique<SettingsButton>("Audio");
	audio_button_->setToggleState(true, NotificationType::dontSendNotification);
	audio_button_->setImage(Drawable::createFromImageData(BinaryData::speaker_svg, BinaryData::speaker_svgSize).get());
	audio_button_->addListener(this);


	general_button_ = std::make_unique<SettingsButton>("General");
	general_button_->setToggleState(false, NotificationType::dontSendNotification);
	general_button_->setImage(Drawable::createFromImageData(BinaryData::settings_svg, BinaryData::settings_svgSize).get());
	general_button_->addListener(this);
	
	help_button_ = std::make_unique<SettingsButton>("Help");
	help_button_->setToggleState(false, NotificationType::dontSendNotification);
	help_button_->setImage(Drawable::createFromImageData(BinaryData::question_svg, BinaryData::question_svgSize).get());
	help_button_->addListener(this);    

	addAndMakeVisible(audio_button_.get());
	addAndMakeVisible(general_button_.get());
	addAndMakeVisible(help_button_.get());

	addAndMakeVisible(audio_settings_pane_);
	addChildComponent(general_settings_pane_);
	addChildComponent(help_settings_pane_);

    setSize(WIDTH, HEIGHT);
}

void FxSettingsDialog::SettingsComponent::resized()
{
	audio_button_->setBounds(BUTTON_X, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT);
	general_button_->setBounds(BUTTON_X, audio_button_->getBottom() + 20, BUTTON_WIDTH, BUTTON_HEIGHT);
	help_button_->setBounds(BUTTON_X, general_button_->getBottom() + 20, BUTTON_WIDTH, BUTTON_HEIGHT);

	juce::Rectangle<int> pane_rect(SEPARATOR_X + 1, 1, getWidth() - SEPARATOR_X + 1, getHeight() - 1);
	audio_settings_pane_.setBounds(pane_rect);
	general_settings_pane_.setBounds(pane_rect);
	help_settings_pane_.setBounds(pane_rect);
}

void  FxSettingsDialog::SettingsComponent::buttonClicked(Button* button)
{
	if (button == audio_button_.get())
	{
		button->setToggleState(true, NotificationType::dontSendNotification);
		general_button_->setToggleState(false, NotificationType::dontSendNotification);
		help_button_->setToggleState(false, NotificationType::dontSendNotification);

		audio_settings_pane_.setVisible(true);
		general_settings_pane_.setVisible(false);
		help_settings_pane_.setVisible(false);
	}
	else if (button == general_button_.get())
	{
		button->setToggleState(true, NotificationType::dontSendNotification);
		audio_button_->setToggleState(false, NotificationType::dontSendNotification);
		help_button_->setToggleState(false, NotificationType::dontSendNotification);

		general_settings_pane_.setVisible(true);
		audio_settings_pane_.setVisible(false);
		help_settings_pane_.setVisible(false);
	}
	else if (button == help_button_.get())
	{
		button->setToggleState(true, NotificationType::dontSendNotification);
		audio_button_->setToggleState(false, NotificationType::dontSendNotification);
		general_button_->setToggleState(false, NotificationType::dontSendNotification);

		help_settings_pane_.setVisible(true);
		audio_settings_pane_.setVisible(false);
		general_settings_pane_.setVisible(false);
	}
}

FxSettingsDialog::SettingsPane::SettingsPane(String name)
{
	name_ = name;
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

    title_.setFont(theme.getTitleFont());
	title_.setText(TRANS(name_), NotificationType::dontSendNotification);	
	title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	title_.setJustificationType(Justification::centredLeft);
	addAndMakeVisible(title_);
}

void FxSettingsDialog::SettingsPane::paint(Graphics&)
{
    auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

    title_.setFont(theme.getTitleFont());
    title_.setText(TRANS(name_), NotificationType::dontSendNotification);    
}

namespace
{
	// Slider positions 0-3 map directly to DfxDsp::BrickwallSteepness (also
	// 0-3). Position 4 is a UI-only concept - linear-phase (FIR) mode - which
	// is a separate bool on the controller/DSP layer, not a BrickwallSteepness
	// value; see updateBrickwallSteepnessLabel() and the slider's onValueChange
	// for how position 4 is translated. Computed fresh on every call (not
	// cached) so the text refreshes correctly if the display language changes,
	// matching this file's existing TRANS()-at-point-of-use convention (see
	// setText()).
	constexpr int BRICKWALL_LINEAR_PHASE_SLIDER_POSITION = 4;

	String brickwallSteepnessLabel(int position)
	{
		switch (position)
		{
		case DfxDsp::BrickwallSteepness::Gentle:
			return TRANS("Gentle (~12 dB/octave)");
		case DfxDsp::BrickwallSteepness::Standard:
			return TRANS("Standard (~48 dB/octave)");
		case DfxDsp::BrickwallSteepness::Steep:
			return TRANS("Steep (~96 dB/octave)");
		case DfxDsp::BrickwallSteepness::UltraSteep:
			return TRANS("Ultra Steep (~132 dB/octave)");
		case BRICKWALL_LINEAR_PHASE_SLIDER_POSITION:
		default:
			return TRANS("Linear Phase (~20ms added latency)");
		}
	}

	String brickwallSteepnessTooltip(int position)
	{
		switch (position)
		{
		case DfxDsp::BrickwallSteepness::Gentle:
			return TRANS("Gentlest rolloff. Lowest CPU use, but lets more content below 20Hz and above 20kHz through.");
		case DfxDsp::BrickwallSteepness::Standard:
			return TRANS("Balanced rolloff and CPU use. Recommended for most listening.");
		case DfxDsp::BrickwallSteepness::Steep:
			return TRANS("Steep rolloff, at the cost of more CPU use and more phase distortion right at the edges of the band.");
		case DfxDsp::BrickwallSteepness::UltraSteep:
			return TRANS("Steepest rolloff and closest to a true brickwall, at the cost of the most CPU use and the most phase distortion right at the edges of the band.");
		case BRICKWALL_LINEAR_PHASE_SLIDER_POSITION:
		default:
			return TRANS("Zero phase distortion, at the cost of noticeable added latency to all audio while enabled - may cause video/game audio to drift out of sync.");
		}
	}

	// The slider is the single UI control for two independent controller-side
	// concepts (steepness enum, linear-phase bool) - this combines them into
	// the one slider position that should be displayed.
	int currentBrickwallSliderPosition(FxController& controller)
	{
		if (controller.isBrickwallFilterLinearPhaseOn())
		{
			return BRICKWALL_LINEAR_PHASE_SLIDER_POSITION;
		}

		return static_cast<int>(controller.getBrickwallFilterSteepness());
	}
}

FxSettingsDialog::AudioSettingsPane::AudioSettingsPane() :
	SettingsPane("Audio"),
	prioritize_new_output_toggle_(TRANS("Prioritize new output devices")),
	brickwall_filter_toggle_(TRANS("Brickwall Filter (20Hz - 20kHz)")),
	reset_presets_button_(TRANS("Reset presets to factory defaults"))
{
	setFocusContainer(true);

	output_preference_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	output_preference_title_.setJustificationType(Justification::centredLeft);

	output_preference_.setMouseCursor(MouseCursor::PointingHandCursor);
	output_preference_.setWantsKeyboardFocus(true);
	output_preference_.setEnabled(true);

	prioritize_new_output_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	prioritize_new_output_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	prioritize_new_output_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	prioritize_new_output_toggle_.setWantsKeyboardFocus(true);

	reset_presets_button_.setSize(RESET_PRESETS_BUTTON_WIDTH, BUTTON_HEIGHT);
	reset_presets_button_.setMouseCursor(MouseCursor::PointingHandCursor);

	prioritize_new_output_toggle_.setToggleState(FxController::getInstance().isNewOutputPrioritized(), NotificationType::dontSendNotification);
	prioritize_new_output_toggle_.onClick = [this]() { FxController::getInstance().setNewOutputPrioritized(prioritize_new_output_toggle_.getToggleState()); };

	brickwall_filter_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	brickwall_filter_title_.setJustificationType(Justification::centredLeft);

	brickwall_filter_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	brickwall_filter_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	brickwall_filter_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	brickwall_filter_toggle_.setWantsKeyboardFocus(true);
	brickwall_filter_toggle_.setToggleState(FxController::getInstance().isBrickwallFilterOn(), NotificationType::dontSendNotification);
	brickwall_filter_toggle_.onClick = [this]() {
		FxController::getInstance().setBrickwallFilterOn(brickwall_filter_toggle_.getToggleState());
		updateBrickwallControlsEnabled();
		};

	brickwall_steepness_slider_.setSliderStyle(Slider::LinearHorizontal);
	brickwall_steepness_slider_.setRange(0, 4, 1);
	brickwall_steepness_slider_.setTextBoxStyle(Slider::NoTextBox, false, 0, 0);
	brickwall_steepness_slider_.setMouseCursor(MouseCursor::PointingHandCursor);
	brickwall_steepness_slider_.setWantsKeyboardFocus(true);
	brickwall_steepness_slider_.setValue(static_cast<double>(currentBrickwallSliderPosition(FxController::getInstance())), NotificationType::dontSendNotification);
	brickwall_steepness_slider_.onValueChange = [this]() {
		auto& controller = FxController::getInstance();
		int position = static_cast<int>(brickwall_steepness_slider_.getValue());

		if (position == BRICKWALL_LINEAR_PHASE_SLIDER_POSITION)
		{
			controller.setBrickwallFilterLinearPhaseOn(true);
		}
		else
		{
			controller.setBrickwallFilterLinearPhaseOn(false);
			controller.setBrickwallFilterSteepness(static_cast<DfxDsp::BrickwallSteepness>(position));
		}

		updateBrickwallSteepnessLabel();
		};

	brickwall_steepness_value_label_.setJustificationType(Justification::centredLeft);
	brickwall_steepness_position_label_.setJustificationType(Justification::centredLeft);
	brickwall_steepness_position_label_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	updateBrickwallSteepnessLabel();

	brickwall_preview_button_.setTooltip(TRANS("Plays back only the content the filter is removing, so you can hear what it affects."));
	brickwall_preview_button_.onClick = [this]() {
		auto& controller = FxController::getInstance();
		controller.setBrickwallFilterPreviewOn(brickwall_preview_button_.getToggleState());
		};

	updateBrickwallControlsEnabled();

	auto preset_modified = false;
	auto preset_count = FxModel::getModel().getPresetCount();
	for (auto i = 0; i < preset_count; i++)
	{
		if (FxModel::getModel().isPresetModified(i))
		{
			preset_modified = true;
			break;
		}
	}
	reset_presets_button_.setEnabled(FxModel::getModel().getUserPresetCount() > 0 || preset_modified);

	reset_presets_button_.onClick = [this]() {
		auto& controller = FxController::getInstance();

		controller.resetPresets();
		reset_presets_button_.setEnabled(false);
		};

	setText();

	addAndMakeVisible(&output_preference_title_);
	addAndMakeVisible(&output_preference_);
	addAndMakeVisible(&prioritize_new_output_toggle_);
	addAndMakeVisible(&brickwall_filter_title_);
	addAndMakeVisible(&brickwall_filter_toggle_);
	addAndMakeVisible(&brickwall_steepness_slider_);
	addAndMakeVisible(&brickwall_steepness_value_label_);
	addAndMakeVisible(&brickwall_steepness_position_label_);
	addAndMakeVisible(&brickwall_preview_button_);
	addAndMakeVisible(&reset_presets_button_);
}

void FxSettingsDialog::AudioSettingsPane::updateBrickwallControlsEnabled()
{
	bool filter_on = brickwall_filter_toggle_.getToggleState();

	brickwall_steepness_slider_.setEnabled(filter_on);
	brickwall_preview_button_.setEnabled(filter_on);

	if (!filter_on && brickwall_preview_button_.getToggleState())
	{
		brickwall_preview_button_.setToggleState(false, NotificationType::dontSendNotification);
	}
}

void FxSettingsDialog::AudioSettingsPane::updateBrickwallSteepnessLabel()
{
	int position = static_cast<int>(brickwall_steepness_slider_.getValue());

	brickwall_steepness_value_label_.setText(brickwallSteepnessLabel(position), NotificationType::dontSendNotification);
	brickwall_steepness_slider_.setTooltip(brickwallSteepnessTooltip(position));

	static constexpr int BRICKWALL_SLIDER_NUM_POSITIONS = BRICKWALL_LINEAR_PHASE_SLIDER_POSITION + 1;
	brickwall_steepness_position_label_.setText(String(position + 1) + "/" + String(BRICKWALL_SLIDER_NUM_POSITIONS), NotificationType::dontSendNotification);

	auto pos = brickwall_steepness_slider_.getPositionOfValue(brickwall_steepness_slider_.getValue());
	auto slider_bounds = brickwall_steepness_slider_.getBounds();
	auto x = jmin(slider_bounds.getX() + (int)pos + FxTheme::SLIDER_THUMB_RADIUS, slider_bounds.getRight() - STEEPNESS_VALUE_LABEL_WIDTH);
	brickwall_steepness_value_label_.setBounds(x, slider_bounds.getBottom() + 2, STEEPNESS_VALUE_LABEL_WIDTH, LABEL_HEIGHT);
}

FxSettingsDialog::AudioSettingsPane::~AudioSettingsPane()
{
}

void FxSettingsDialog::AudioSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

	output_preference_title_.setBounds(X_MARGIN, ENDPOINT_Y, LABEL_WIDTH, LABEL_HEIGHT);
	int y = output_preference_title_.getBottom() + 10;
	auto width = getWidth() - ((X_MARGIN + 5) * 2);
	output_preference_.setBounds(X_MARGIN, y, width, OUTPUT_PREFERENCE_HEIGHT);

    y = output_preference_.getBottom() + 10;
    prioritize_new_output_toggle_.setBounds(X_MARGIN, y, width, TOGGLE_BUTTON_HEIGHT);

	auto group_x = output_preference_title_.getX() - GROUP_MARGIN;
	auto group_y = output_preference_title_.getY() - GROUP_MARGIN;
	auto group_width = output_preference_.getRight() - group_x + GROUP_MARGIN;
	auto group_height = prioritize_new_output_toggle_.getBottom() - group_y + GROUP_MARGIN;
	output_preference_bounds_ = juce::Rectangle<float>(group_x, group_y, group_width, group_height);

	y = prioritize_new_output_toggle_.getBottom() + 30;
	brickwall_filter_title_.setBounds(X_MARGIN, y, LABEL_WIDTH, LABEL_HEIGHT);

	y = brickwall_filter_title_.getBottom() + 10;
	brickwall_filter_toggle_.setBounds(X_MARGIN, y, width, TOGGLE_BUTTON_HEIGHT);

	y = brickwall_filter_toggle_.getBottom() + 10;
	auto steepness_indent = X_MARGIN + 20;
	auto steepness_width = width - 20;
	brickwall_preview_button_.setBounds(steepness_indent, y, PREVIEW_BUTTON_WIDTH, SLIDER_HEIGHT);

	auto slider_x = steepness_indent + PREVIEW_BUTTON_WIDTH + PREVIEW_BUTTON_GAP;
	auto slider_width = steepness_width - PREVIEW_BUTTON_WIDTH - PREVIEW_BUTTON_GAP - POSITION_LABEL_WIDTH - POSITION_LABEL_GAP;
	brickwall_steepness_slider_.setBounds(slider_x, y, slider_width, SLIDER_HEIGHT);

	brickwall_steepness_position_label_.setBounds(brickwall_steepness_slider_.getRight() + POSITION_LABEL_GAP, y, POSITION_LABEL_WIDTH, SLIDER_HEIGHT);

	updateBrickwallSteepnessLabel();

	auto brickwall_group_x = brickwall_filter_title_.getX() - GROUP_MARGIN;
	auto brickwall_group_y = brickwall_filter_title_.getY() - GROUP_MARGIN;
	auto brickwall_group_width = width + GROUP_MARGIN * 2;
	auto brickwall_group_height = brickwall_steepness_value_label_.getBottom() - brickwall_group_y + GROUP_MARGIN;
	brickwall_group_bounds_ = juce::Rectangle<float>(brickwall_group_x, brickwall_group_y, brickwall_group_width, brickwall_group_height);

	y = brickwall_steepness_value_label_.getBottom() + 30;
	resizeResetButton(X_MARGIN, y);
}

void FxSettingsDialog::AudioSettingsPane::paint(Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));

	g.setFillType(FillType(Colour(FXCOLOR(DefaultFill)).withAlpha(0.2f)));
	g.fillRoundedRectangle(output_preference_bounds_, 8);
	g.fillRoundedRectangle(brickwall_group_bounds_, 8);

	setText();

	SettingsPane::paint(g);
}

void FxSettingsDialog::AudioSettingsPane::setText()
{
	auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

	output_preference_title_.setFont(theme.getNormalFont());
	output_preference_title_.setText(TRANS("Output Device Preference"), NotificationType::dontSendNotification);

	prioritize_new_output_toggle_.setButtonText(TRANS("Prioritize new output devices"));

	brickwall_filter_title_.setFont(theme.getNormalFont());
	brickwall_filter_title_.setText(TRANS("Bandwidth Filter"), NotificationType::dontSendNotification);
	brickwall_filter_toggle_.setButtonText(TRANS("Brickwall Filter (20Hz - 20kHz)"));
	updateBrickwallSteepnessLabel();

	reset_presets_button_.setButtonText(TRANS("Reset presets to factory defaults"));
	resizeResetButton(reset_presets_button_.getX(), reset_presets_button_.getY());
}

void FxSettingsDialog::AudioSettingsPane::resizeResetButton(int x, int y)
{
	String buttonText = reset_presets_button_.getButtonText();

	int index = 0;
	int lineCount = 1;
	do {
		index = buttonText.indexOfChar(index, L'\n');
		if (index >= 0)
		{
			index++;
			lineCount++;
		}
		else
		{
			break;
		}
	} while (lineCount <= 3); // Resize the button height for upto 3 lines of text

	int buttonWidth = min(reset_presets_button_.getBestWidthForHeight(BUTTON_HEIGHT * lineCount), MAX_BUTTON_WIDTH);
	if (buttonWidth < RESET_PRESETS_BUTTON_WIDTH)
	{
		buttonWidth = RESET_PRESETS_BUTTON_WIDTH;
	}

	reset_presets_button_.setBounds(x, y, buttonWidth, BUTTON_HEIGHT * lineCount);
}

void FxSettingsDialog::AudioSettingsPane::visibilityChanged()
{
	if (isVisible())
	{
		output_preference_.update();

		auto& controller = FxController::getInstance();

		brickwall_filter_toggle_.setToggleState(controller.isBrickwallFilterOn(), NotificationType::dontSendNotification);

		brickwall_steepness_slider_.setValue(static_cast<double>(currentBrickwallSliderPosition(controller)), NotificationType::dontSendNotification);
		updateBrickwallSteepnessLabel();

		brickwall_preview_button_.setToggleState(controller.isBrickwallFilterPreviewOn(), NotificationType::dontSendNotification);

		updateBrickwallControlsEnabled();
    }
}

void FxSettingsDialog::AudioSettingsPane::mouseEnter(const MouseEvent& mouse_event)
{
	Component::mouseEnter(mouse_event);
}

void FxSettingsDialog::AudioSettingsPane::mouseExit(const MouseEvent& mouse_event)
{
	Component::mouseEnter(mouse_event);
}

FxSettingsDialog::GeneralSettingsPane::GeneralSettingsPane() :
	SettingsPane("General Preferences"),
	launch_toggle_(TRANS("Launch on system startup")),
	hide_help_tips_toggle_(TRANS("Hide help tips for audio controls")),
	hide_notifications_toggle_(TRANS("Hide notifications")),
	hotkeys_toggle_(TRANS("Disable keyboard shortcuts"))
{
	StringArray hotKeySettingsKeys = { FxController::HK_CMD_ON_OFF, FxController::HK_CMD_OPEN_CLOSE, FxController::HK_CMD_NEXT_PRESET, FxController::HK_CMD_PREVIOUS_PRESET, FxController::HK_CMD_NEXT_OUTPUT };
	StringArray hotkey_names = { "Turn FxSound On/Off", "Open/Close FxSound",
								   "Use Next Preset", "Use Previous Preset", "Change Playback Device"};

	setFocusContainer(true);

	launch_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	launch_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	launch_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	launch_toggle_.setWantsKeyboardFocus(true);
	
	hide_help_tips_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
    hide_help_tips_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
    hide_help_tips_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hide_help_tips_toggle_.setWantsKeyboardFocus(true);
	hide_notifications_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	hide_notifications_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hide_notifications_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hide_notifications_toggle_.setWantsKeyboardFocus(true);
	hotkeys_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	hotkeys_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hotkeys_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hotkeys_toggle_.setWantsKeyboardFocus(true);

	bool hotkey_enabled = FxModel::getModel().getHotkeySupport();
	for (int i=0; i<hotkey_names.size(); i++)
	{
		auto label = new FxHotkeyLabel(hotkey_names[i], hotKeySettingsKeys[i]);
		label->setEnabled(hotkey_enabled);
		hotkey_labels_.add(label);
		addAndMakeVisible(label);
	}

    if (SysInfo::canSupportHotkeys())
    {
        hotkeys_toggle_.setToggleState(!FxModel::getModel().getHotkeySupport(), NotificationType::dontSendNotification);
    }
    else
    {
        hotkeys_toggle_.setToggleState(true, NotificationType::dontSendNotification);
        hotkeys_toggle_.setEnabled(false);
    }
	hotkeys_toggle_.onClick = [this]()  { 
											FxController::getInstance().enableHotkeys(!hotkeys_toggle_.getToggleState());
											bool enabled = FxModel::getModel().getHotkeySupport();
											for (auto& hotkey_label : hotkey_labels_)
											{
												hotkey_label->setEnabled(enabled);
											}
										};

	launch_toggle_.setToggleState(FxController::getInstance().isLaunchOnStartup(), NotificationType::dontSendNotification);
	launch_toggle_.onClick = [this]() { FxController::getInstance().setLaunchOnStartup(launch_toggle_.getToggleState()); };

    hide_help_tips_toggle_.setToggleState(FxController::getInstance().isHelpTooltipsHidden(), NotificationType::dontSendNotification);
    hide_help_tips_toggle_.onClick = [this]() { FxController::getInstance().setHelpTooltipsHidden(hide_help_tips_toggle_.getToggleState()); };
	
	hide_notifications_toggle_.setToggleState(FxController::getInstance().isNotificationsHidden(), NotificationType::dontSendNotification);
	hide_notifications_toggle_.onClick = [this]() { FxController::getInstance().setNotificationsHidden(hide_notifications_toggle_.getToggleState()); };

	auto os = SystemStats::getOperatingSystemType();
	if (os == SystemStats::OperatingSystemType::Windows7)
	{
		addAndMakeVisible(&launch_toggle_);
	}
	
	addAndMakeVisible(&hide_help_tips_toggle_);
	addAndMakeVisible(&hide_notifications_toggle_);
	addAndMakeVisible(&hotkeys_toggle_);
	addAndMakeVisible(&language_switch_);

	setText();
}

FxSettingsDialog::GeneralSettingsPane::~GeneralSettingsPane()
{
}

void FxSettingsDialog::GeneralSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

    language_switch_.setBounds(X_MARGIN, LANGUAGE_SWITCH_Y, FxLanguage::WIDTH, FxLanguage::HEIGHT);

    int y = language_switch_.getBottom() + 20;
	if (launch_toggle_.isVisible())
	{
		launch_toggle_.setBounds(X_MARGIN, y, getWidth() - X_MARGIN, TOGGLE_BUTTON_HEIGHT);
		y = launch_toggle_.getBottom() + 20;
	}

    hide_help_tips_toggle_.setBounds(X_MARGIN, y, getWidth() - X_MARGIN, TOGGLE_BUTTON_HEIGHT);

	y = hide_help_tips_toggle_.getBottom() + 10;
	hide_notifications_toggle_.setBounds(X_MARGIN, y, getWidth() - X_MARGIN, TOGGLE_BUTTON_HEIGHT);

    y = hide_notifications_toggle_.getBottom() + 10;
	hotkeys_toggle_.setBounds(X_MARGIN, y, getWidth()-X_MARGIN, TOGGLE_BUTTON_HEIGHT);

	y = hotkeys_toggle_.getBottom() + 5;
	for (auto hotkey_label : hotkey_labels_)
	{
		hotkey_label->setBounds(HOTKEY_LABEL_X, y, getWidth()-HOTKEY_LABEL_X, HOTKEY_LABEL_HEIGHT);
		y += HOTKEY_LABEL_HEIGHT + 10;
	}
}

void FxSettingsDialog::GeneralSettingsPane::paint(Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));

    setText();

	SettingsPane::paint(g);    
}

void FxSettingsDialog::GeneralSettingsPane::setText()
{
    auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

    int height = FxSettingsDialog::SettingsComponent::HEIGHT;
    launch_toggle_.setButtonText(TRANS("Launch on system startup"));
    hide_help_tips_toggle_.setButtonText(TRANS("Hide help tips for audio controls"));
	hide_notifications_toggle_.setButtonText(TRANS("Hide notifications"));

    hotkeys_toggle_.setButtonText(TRANS("Disable keyboard shortcuts"));
}

FxSettingsDialog::HelpSettingsPane::HelpSettingsPane() : SettingsPane("Help"), auto_updates_toggle_(TRANS("Automatic updates"))
{	
	version_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	version_title_.setJustificationType(Justification::centredLeft);
	version_text_.setJustificationType(Justification::centredLeft);	
	support_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	support_title_.setJustificationType(Justification::centredLeft);	
	maintenance_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	maintenance_title_.setJustificationType(Justification::centredLeft);

	changelog_link_.setURL(URL(L"https://www.fxsound.com/changelog"));
	changelog_link_.setJustificationType(Justification::topLeft);
	quicktour_link_.setJustificationType(Justification::topLeft);
	submitlogs_link_.setJustificationType(Justification::topLeft);
	helpcenter_link_.setURL(URL(L"https://www.fxsound.com/learning-center"));
	helpcenter_link_.setJustificationType(Justification::topLeft);
    feedback_link_.setURL(URL("https://james722808.typeform.com/to/QfEP5QrP"));
	feedback_link_.setJustificationType(Justification::topLeft);

	auto_updates_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	auto_updates_toggle_.setToggleState(FxController::getInstance().getAutoUpdates(), NotificationType::dontSendNotification);
	auto_updates_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	auto_updates_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));

	auto_updates_toggle_.onClick = [this]() {
		FxController::getInstance().setAutoUpdates(auto_updates_toggle_.getToggleState());
	};

    setText();

	addAndMakeVisible(version_title_);
	addAndMakeVisible(version_text_);
	addAndMakeVisible(support_title_);
	addAndMakeVisible(maintenance_title_);
	addAndMakeVisible(changelog_link_);
	addChildComponent(quicktour_link_);
	addChildComponent(submitlogs_link_);
	addAndMakeVisible(helpcenter_link_);
	addAndMakeVisible(auto_updates_toggle_);
}

void FxSettingsDialog::HelpSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

	version_title_.setBounds(X_MARGIN, TEXT_Y, getWidth()-X_MARGIN, TITLE_HEIGHT);
	version_text_.setBounds(X_MARGIN, version_title_.getBottom()+10, getWidth()-X_MARGIN, TEXT_HEIGHT);
	changelog_link_.setBounds(X_MARGIN+5, version_text_.getBottom()+10, getWidth()-X_MARGIN, HYPERLINK_HEIGHT);
	support_title_.setBounds(X_MARGIN, changelog_link_.getBottom()+20, getWidth()-X_MARGIN, TITLE_HEIGHT);
	helpcenter_link_.setBounds(X_MARGIN+5, support_title_.getBottom()+10, getWidth()-X_MARGIN, HYPERLINK_HEIGHT);
	maintenance_title_.setBounds(X_MARGIN, helpcenter_link_.getBottom()+20, getWidth()-X_MARGIN, TITLE_HEIGHT);
	auto_updates_toggle_.setBounds(X_MARGIN + 5, maintenance_title_.getBottom() + 10, BUTTON_WIDTH, TOGGLE_BUTTON_HEIGHT);
}

void FxSettingsDialog::HelpSettingsPane::paint(Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));

    setText();

	SettingsPane::paint(g);
}

void FxSettingsDialog::HelpSettingsPane::setText()
{
    auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

    version_title_.setText(TRANS("Version"), NotificationType::dontSendNotification);
    version_title_.setFont(theme.getNormalFont());
    
    version_text_.setText(L"v" + JUCEApplication::getInstance()->getApplicationVersion(), NotificationType::dontSendNotification);
    version_text_.setFont(theme.getSmallFont());
    
    support_title_.setText(TRANS("Support"), NotificationType::dontSendNotification);
    support_title_.setFont(theme.getNormalFont());
    
    maintenance_title_.setText(TRANS("Maintenance"), NotificationType::dontSendNotification);
    maintenance_title_.setFont(theme.getNormalFont());

    changelog_link_.setButtonText(TRANS("Changelog"));    
    quicktour_link_.setButtonText(TRANS("Quick tour"));    
    submitlogs_link_.setButtonText(TRANS("Submit debug logs"));    
    helpcenter_link_.setButtonText(TRANS("Help center"));        
    feedback_link_.setButtonText(TRANS("Feedback"));
	auto_updates_toggle_.setButtonText(TRANS("Automatic updates"));;
}
