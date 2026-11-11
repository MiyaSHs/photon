-- Client: 3 s after this runs, record the time between frames for 20 s, then print the mean, percentiles and 1% / 0.1% lows (the
-- mean frame time of the slowest 1% / 0.1% of frames), the same way for stock and Photon. Uses SysTime, not the engine's frame time.
local dts, last, t0 = {}, nil, nil
timer.Simple(3, function()
	t0 = SysTime()
	hook.Add("Think", "photon_perf", function()
		local now = SysTime()
		if last then dts[#dts + 1] = (now - last) * 1000 end
		last = now
		if now - t0 >= 20 then
			hook.Remove("Think", "photon_perf")
			table.sort(dts)
			local n, sum = #dts, 0
			for _, v in ipairs(dts) do sum = sum + v end
			local function pct(p) return dts[math.max(1, math.min(n, math.ceil(n * p)))] end
			local function low(f)
				local k, s = math.max(1, math.floor(n * f)), 0
				for i = n - k + 1, n do s = s + dts[i] end
				return s / k
			end
			print(string.format("PHOTON_PERF frames %d mean_ms %.3f p50 %.3f p99 %.3f p999 %.3f max %.3f fps %.1f low1_ms %.3f low01_ms %.3f mcore %s queue %s",
				n, sum / n, pct(0.5), pct(0.99), pct(0.999), dts[n], n / 20, low(0.01), low(0.001),
				GetConVarString("gmod_mcore_test"), GetConVarString("mat_queue_mode")))
		end
	end)
end)
