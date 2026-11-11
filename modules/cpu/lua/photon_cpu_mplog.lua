-- Photon CPU multiplayer log (analysis/17): an opt-in log of what makes multiplayer feel laggy, for real sessions between Photon
-- players (the user's idea, 2026-11-10: "when i do play, i get my friend the mod installed ... and i enable some kind of heavy logging").
-- Setting photon_cpu_mplog 1 (Photon menu, data/photon/settings.txt; tests: photon_cpu_mplog=1 in data/photon_harness.txt).
-- Once a second, one CSV line per side into data/photon/mplog/<date>_<time>_sv.txt (the host) and _cl.txt (each player):
--   server: real ticks run that second, the longest real-time gap between two ticks and how many gaps exceeded 50 ms (ticks run in
--           bursts = everyone's input handled in clumps), phys_timescale (the physics governor), corpses and dropped weapons, and per
--           player (by player number, never names): usercmds received, the longest real gap between two of them, ping, packet loss;
--   client: frames, average and worst frame time, the server's frame time as the engine reports it, ping, and snap-backs (frames where
--           the local player's position jumped far beyond its velocity: the server correcting the prediction).
-- Capped at 20 MB a file. Costs a few hooks; off by default.
if SERVER then AddCSLuaFile() end

local function enabled()
	local h = file.Exists("photon_harness.txt", "DATA") and (file.Read("photon_harness.txt", "DATA") or "") or ""
	local v = h:match("photon_cpu_mplog=(%d)")
	if not v then v = ("\n" .. (file.Read("photon/settings.txt", "DATA") or "")):match("\nphoton_cpu_mplog[ \t]+(%d)") end
	return v == "1"
end

local CAP = 20 * 1024 * 1024
local path, written
local function open_log(side, header)
	file.CreateDir("photon/mplog")
	path = "photon/mplog/" .. os.date("%Y%m%d_%H%M%S") .. "_" .. side .. ".txt"
	file.Write(path, header .. "\n")
	written = #header + 1
	Msg("Photon multiplayer log: garrysmod/data/" .. path .. "\n")
end
local function line(s)
	if not path or written > CAP then return end
	file.Append(path, s .. "\n")
	written = written + #s + 1
end

local on = false
local function start(side, header, setup)
	if on then return end
	on = true
	open_log(side, header)
	setup()
end

if SERVER then
	local ticks, last_tick, gap_max, gaps50 = 0, nil, 0, 0
	local cmds = {}   -- player entity -> { n, last, gap_max }
	local t0 = SysTime()
	local function setup()
		hook.Add("Tick", "photon_cpu_mplog", function()
			local now = SysTime()
			if last_tick then
				local g = (now - last_tick) * 1000
				if g > gap_max then gap_max = g end
				if g > 50 then gaps50 = gaps50 + 1 end
			end
			last_tick, ticks = now, ticks + 1
		end)
		hook.Add("StartCommand", "photon_cpu_mplog", function(ply)
			local now = SysTime()
			local c = cmds[ply]
			if not c then c = { n = 0, gap_max = 0 }; cmds[ply] = c end
			if c.last then
				local g = (now - c.last) * 1000
				if g > c.gap_max then c.gap_max = g end
			end
			c.last, c.n = now, c.n + 1
		end)
		local ts = GetConVar("phys_timescale")
		timer.Create("photon_cpu_mplog", 1, 0, function()
			local t = SysTime() - t0
			local rag = #ents.FindByClass("prop_ragdoll")
			local loot = 0
			for _, w in ipairs(ents.FindByClass("weapon_*")) do if not IsValid(w:GetOwner()) then loot = loot + 1 end end
			line(string.format("S,%.1f,%d,%.1f,%d,%.2f,%d,%d,%d", t, ticks, gap_max, gaps50, ts and ts:GetFloat() or -1, rag, loot,
				#player.GetAll()))
			for _, ply in ipairs(player.GetAll()) do
				local c = cmds[ply] or { n = 0, gap_max = 0 }
				line(string.format("P,%.1f,%d,%s,%d,%.1f,%d,%d", t, ply:EntIndex(), ply:IsBot() and "bot" or (ply:IsListenServerHost() and "host" or "remote"),
					c.n, c.gap_max, ply:Ping(), ply:PacketLoss()))
				c.n, c.gap_max = 0, 0
			end
			ticks, gap_max, gaps50 = 0, 0, 0
		end)
	end
	hook.Add("PlayerDisconnected", "photon_cpu_mplog", function(ply) cmds[ply] = nil end)
	local function try()
		if enabled() and not game.SinglePlayer() then
			start("sv", "# S,t,ticks,tick_gap_max_ms,tick_gaps_over_50ms,phys_timescale,ragdolls,dropped_weapons,players\n" ..
				"# P,t,player_number,kind,usercmds,usercmd_gap_max_ms,ping_ms,packet_loss", setup)
		end
	end
	timer.Simple(3, try)
	timer.Create("photon_cpu_mplog_check", 10, 0, try)   -- turned on from the menu later
	return
end

-- client
local frames, ft_sum, ft_max, snaps, snap_max = 0, 0, 0, 0, 0
local last_pos
local t0 = SysTime()
local function setup()
	hook.Add("Think", "photon_cpu_mplog", function()
		local dt = RealFrameTime()
		frames, ft_sum = frames + 1, ft_sum + dt
		if dt > ft_max then ft_max = dt end
		local lp = LocalPlayer()
		if IsValid(lp) and lp:Alive() then
			local pos = lp:GetPos()
			if last_pos then
				local err = (pos - last_pos - lp:GetVelocity() * dt):Length()   -- movement its velocity doesn't explain
				if err > 32 and lp:GetMoveType() == MOVETYPE_WALK then
					snaps = snaps + 1
					if err > snap_max then snap_max = err end
				end
			end
			last_pos = pos
		else
			last_pos = nil
		end
	end)
	timer.Create("photon_cpu_mplog", 1, 0, function()
		local sft, sdev = engine.ServerFrameTime()
		local lp = LocalPlayer()
		line(string.format("C,%.1f,%d,%.2f,%.2f,%.2f,%.2f,%d,%d,%.0f", SysTime() - t0, frames, frames > 0 and ft_sum / frames * 1000 or 0,
			ft_max * 1000, (sft or 0) * 1000, (sdev or 0) * 1000, IsValid(lp) and lp:Ping() or -1, snaps, snap_max))
		frames, ft_sum, ft_max, snaps, snap_max = 0, 0, 0, 0, 0
	end)
end
local function try()
	if enabled() and not game.SinglePlayer() then
		start("cl", "# C,t,frames,frame_ms_avg,frame_ms_max,server_frame_ms,server_frame_dev_ms,ping_ms,snapbacks,snapback_max_units", setup)
	end
end
timer.Simple(3, try)
timer.Create("photon_cpu_mplog_check", 10, 0, try)
