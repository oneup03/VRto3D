local api = uevr.api
local vr = uevr.params.vr

local VR_WORLD_SCALE = "VR_WorldScale"
local scale_low = '0.25'
local scale_high = '1.0'
local current_scale_is_low = false

-- Change these to whatever button you want
local TOGGLE_BUTTON = XINPUT_GAMEPAD_START
-- XINPUT_GAMEPAD_DPAD_UP
-- XINPUT_GAMEPAD_DPAD_DOWN
-- XINPUT_GAMEPAD_DPAD_LEFT
-- XINPUT_GAMEPAD_DPAD_RIGHT
-- XINPUT_GAMEPAD_START
-- XINPUT_GAMEPAD_BACK
-- XINPUT_GAMEPAD_LEFT_THUMB
-- XINPUT_GAMEPAD_RIGHT_THUMB
-- XINPUT_GAMEPAD_LEFT_SHOULDER
-- XINPUT_GAMEPAD_RIGHT_SHOULDER
-- XINPUT_GAMEPAD_A
-- XINPUT_GAMEPAD_B
-- XINPUT_GAMEPAD_X
-- XINPUT_GAMEPAD_Y


-- State tracking
local key_was_pressed = false
local button_was_pressed = false

-- Apply scale
local function apply_scale()
	if current_scale_is_low then
		vr.set_mod_value(VR_WORLD_SCALE, scale_low)
	else
		vr.set_mod_value(VR_WORLD_SCALE, scale_high)
	end
end

-- XInput gamepad input
uevr.sdk.callbacks.on_xinput_get_state(function(_, user_index, state)
	if user_index ~= 0 or not state or not state.Gamepad then return end
	local button_pressed = (state.Gamepad.wButtons & TOGGLE_BUTTON) ~= 0
	if button_pressed and not button_was_pressed then
		current_scale_is_low = not current_scale_is_low
		apply_scale()
	end
	button_was_pressed = button_pressed
end)

-- Ensure initial value is applied
apply_scale()
