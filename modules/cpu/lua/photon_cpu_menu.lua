-- photon_cpu_menu.lua (Photon CPU): the CPU module's section in Photon's settings window (Photon Render's photon_menu.lua). Client
-- autorun. The values land in data/photon/settings.txt; photon_cpu.dll reads them at game start (an environment variable of the same
-- name, PHOTON_CPU_*, still wins). The switches that are on by default and safe to toggle, and the tick cap (P1, opt-in: it changes
-- game timing under overload).
if not CLIENT then return end

local restart = " Takes effect the next time the game starts."
local live = " Applies within a few seconds."
local section = { "cpu", "Performance (CPU)", {
	{ "photon_cpu_bones", "Character bone reuse", "choice", { { 1, "On" }, { 0, "Off" } },
		"Characters keep the bone positions already computed this frame instead of computing them again (about 20% faster with many NPCs). Same result." .. restart },
	{ "photon_cpu_fscache", "Faster map loading", "choice", { { 1, "On" }, { 0, "Off" } },
		"Remembers which files do not exist, so addon-heavy map loads skip hundreds of thousands of slow disk lookups (about a third faster). Same result." .. restart },
	{ "photon_cpu_physvalid", "Fast physics checks", "choice", { { 1, "On" }, { 0, "Off" } },
		"Speeds up a check GMod makes before every Lua physics call; large gains with active ragdolls and gore mods. Same result." .. restart },
	{ "photon_cpu_physgov", "Slow physics under heavy load", "choice", { { 1, "On (default)" }, { 0, "Off" } },
		"When physics cannot keep up (piles of corpses, debris, explosions), physics objects move in slow motion for a moment while you, NPCs and weapons keep full speed and the frame rate stays up. Physics-driven vehicles slow down too while it lasts." .. restart },
	{ "photon_cpu_tickcap", "Slow down instead of freezing", "choice", { { 0, "Off" }, { 4, "Light" }, { 2, "Strong" }, { 1, "Strongest" } },
		"Single player: when the game cannot keep up (huge battles, gore, piles of corpses), it runs fewer catch-up updates per frame, so it plays in slow motion for a moment instead of dropping to a few frames a second. Changes game timing only while overloaded." .. restart },
	{ "photon_cpu_corpse_collide", "Corpses and dropped loot collide", "choice", { { 1, "Not with each other (default)" }, { 0, "Normally" }, { 2, "Not with players" }, { 3, "Only with the world" } },
		"How the bodies of the dead and the weapons they drop collide. Piles of corpses and loot bumping into each other are a big physics cost; 'Not with each other' keeps them out of each other's way, 'Only with the world' is the lightest." .. live },
	{ "photon_cpu_corpse_light", "Light corpses", "choice", { { 0, "Off" }, { 1, "On" } },
		"Corpses weigh a quarter as much, so you push them aside instead of getting stuck on them (shots also move them more)." .. live },
	{ "photon_cpu_corpse_max", "Most corpses kept", "choice", { { 64, "64 (default)" }, { 0, "No limit" }, { 32, "32" }, { 16, "16" }, { 8, "8" } },
		"Past this many corpses (and as many dropped weapons) the oldest are removed, one at a time. Anything held with the physgun or roped/welded is kept." .. live },
	{ "photon_cpu_corpse_life", "Corpses removed after", "choice", { { 0, "Never" }, { 300, "5 minutes" }, { 120, "2 minutes" }, { 60, "1 minute" }, { 30, "30 seconds" } },
		"Corpses and dropped loot are removed after this long. Anything held with the physgun or roped/welded is kept." .. live },
	{ "photon_cpu_corpse_draw", "Draw corpses and dropped loot", "choice", { { 0, "Always" }, { 1, "Only nearby" }, { 2, "Never" } },
		"Visual only, for you: skips drawing corpses and dropped loot (far away, or at all). Their physics and other players are not affected." .. live },
	{ "photon_cpu_corpse_modfix", "Let ragdoll-mod corpses settle", "choice", { { 1, "On (default)" }, { 0, "Off" } },
		"Ragdoll mods like Fedhoria make corpses stumble for a moment, but leave their controllers behind, so every corpse stays awake and costs physics forever. This ends them when the mod's own stumble time is over, as the mod intends." .. live },
	{ "photon_cpu_mplog", "Multiplayer log", "choice", { { 0, "Off" }, { 1, "On" } },
		"Multiplayer only: once a second, records what makes the game feel laggy (server updates running late or in bursts, how late each player's input is handled, frame times, ping, position snap-backs) into garrysmod/data/photon/mplog. No names are recorded. Starts within 10 seconds; turning it off takes effect on the next map." },
} }

PhotonUI = PhotonUI or {}
if PhotonUI.AddSection then
	PhotonUI.AddSection(section[1], section[2], section[3])
else
	PhotonUI.Pending = PhotonUI.Pending or {}
	table.insert(PhotonUI.Pending, section)
end
