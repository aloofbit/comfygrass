-- Adds a Foliage Density slider to Video -> World Appearance.
--
-- This client's options panel is data-driven: GameOptions is a plain global table and OptionsFrame.lua
-- builds the panel by walking it (see Interface\FrameXML\Options\Options.lua inside patch-9.mpq). So a
-- new slider is one table entry -- no FrameXML edits, no repacked MPQ, and the sliders already there are
-- not touched.
--
-- Why this is not in comfygrass.ini: frillDensity is the CLIENT's setting, not ours. The grass is there
-- whether or not comfygrass is loaded; we only make it move. It belongs in the game's own options, next
-- to the other detail sliders, and it keeps working if you remove the DLL.
--
-- None of the six stock sliders touches it. Environment Detail is the natural guess and it is smallCull,
-- which culls small objects by distance -- nothing to do with how much grass is planted.

COMFYGRASS_FOLIAGE_DENSITY = "Foliage Density";

local ENTRY = {
	-- name is a KEY, not a string: the panel does _G[option.name] to get the label.
	name   = "COMFYGRASS_FOLIAGE_DENSITY",
	desc   = "How much grass and ground clutter the world plants. Costs frame rate, not memory.",
	type   = "slider",
	cvar   = "frillDensity",

	-- The client plants min(frillDensity * 64, 8192) doodads, so it stops changing anything at 128 and a
	-- slider that ran to the CVar's real maximum of 256 would spend half its travel doing nothing.
	minval = 8,
	maxval = 128,
	step   = 8,
	numberLabels = 1,
};

local function AddFoliageSlider()
	if type(GameOptions) ~= "table" or not WORLD_APPEARANCE then
		return false;
	end

	for _, category in ipairs(GameOptions) do
		if category.name == WORLD_APPEARANCE and category.options then
			-- Sit with the other detail sliders, just after Environment Detail; fall back to the end if
			-- that one ever moves or goes away.
			local at = table.getn(category.options) + 1;

			for i, option in ipairs(category.options) do
				if option.cvar == "frillDensity" then
					return true;    -- already present, nothing to do
				end
				if option.cvar == "smallCull" then
					at = i + 1;
				end
			end

			table.insert(category.options, at, ENTRY);
			return true;
		end
	end

	return false;
end

-- VARIABLES_LOADED is late enough that FrameXML's globals and GameOptions exist, and early enough that
-- the options panel has not been opened yet. The panel rebuilds itself from the table every time it is
-- shown anyway, so inserting later would still take -- this is just the tidy moment.
local frame = CreateFrame("Frame");
frame:RegisterEvent("VARIABLES_LOADED");
frame:SetScript("OnEvent", function()
	if not AddFoliageSlider() then
		-- Quietly doing nothing is the right failure here: a client whose options panel is built some
		-- other way is not broken, it just has no table for us to add to.
		DEFAULT_CHAT_FRAME:AddMessage(
			"|cff88cc88comfygrass|r: this client's options panel is not the data-driven one, "
			.. "so no Foliage Density slider was added. Use |cffffff78/console frilldensity 32|r instead.");
	end
end);
