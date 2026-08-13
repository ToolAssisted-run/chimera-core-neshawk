-- Frontend witness: run the core inside miniHawk for a fixed number of frames with nothing
-- pressed, then dump the machine's 2KB of work RAM. The driver compares that dump against the
-- native reference run (run-native --blank), which is what proves the frontend path - package
-- discovery, the generic waterbox adapter, the settings channel, the input chain - lands the same
-- machine the core-level gate signs off on.
--
-- Job description comes from the file named by the MINIHAWK_JOB env var:
--   frames=<how many frames to advance>
--   out=<path to write the final RAM dump (binary)>
--   meta=<path to write result metadata (text)>
--   shot=<optional path to write a screenshot, for checks whose effect is only visible>

local function writeAll(path, data)
	local f = assert(io.open(path, "wb"))
	f:write(data)
	f:close()
end

local meta = {}
local function finish(status, detail)
	local lines = {
		"status=" .. status,
		"detail=" .. (detail or ""),
		"frames=" .. (meta.frames or 0),
		"lag=" .. (meta.lag or 0),
	}
	if meta.metaPath then
		writeAll(meta.metaPath, table.concat(lines, "\n") .. "\n")
	end
	client.exit()
end

local jobPath = os.getenv("MINIHAWK_JOB")
if jobPath == nil then
	error("MINIHAWK_JOB env var not set")
end
local job = {}
for line in io.lines(jobPath) do
	local k, v = line:match("^([^=]+)=(.*)$")
	if k then job[k] = v end
end
meta.metaPath = job.meta

if emu.getsystemid() ~= "NES" then
	finish("ERROR", "wrong system id: " .. tostring(emu.getsystemid()))
end
-- Several packages can claim the NES, and the rom opens with whichever one the config prefers.
-- Without this check the gate happily measures a different emulator and passes.
if emu.getcorename() ~= "QuickerNesHawk" then
	finish("ERROR", "wrong core: " .. tostring(emu.getcorename()))
end

pcall(function() client.speedmode(6400) end)
pcall(function() client.invisibleemulation(true) end)

local frames = tonumber(job.frames) or 600
for _ = 1, frames do
	emu.frameadvance()
end

meta.frames = emu.framecount()
meta.lag = emu.lagcount()

if job.shot ~= nil and job.shot ~= "" then
	client.screenshot(job.shot)
end

local ram = memory.read_bytes_as_array(0, 0x800, "RAM")
local chunks = {}
for i = 1, #ram do
	chunks[i] = string.char(ram[i])
end
writeAll(job.out, table.concat(chunks))

finish("OK", "")
