-- Autoboot script: spins the V33 at its reset vector, then writes each state file into the M92 video hardware through the
-- real memory handlers and takes a snapshot. List of files in the environment variable NBB_INJECT (space separated).
if _G.gf2_loaded then return end   -- MAME runs the autoboot script again after a soft reset
_G.gf2_loaded = true
local files = {}
for f in string.gmatch(os.getenv("NBB_INJECT"), "%S+") do files[#files + 1] = f end
local n, idx, stage = 0, 1, 0
local function u16(s, o) return string.unpack("<I2", s, o + 1) end
local function write_state(path)
    local cpu = manager.machine.devices[":maincpu"]
    local prog, io_s = cpu.spaces["program"], cpu.spaces["io"]
    local h = io.open(path, "rb"); local s = h:read("a"); h:close()
    local REGS = 0x10000
    local vc = u16(s, REGS + 32)
    prog:write_u16(0xf9002, 3)                                  -- CPU reaches the sprite buffer and the palette directly
    for i = 0, 0xfffe, 2 do prog:write_u16(0xd0000 + i, u16(s, i)) end
    local P = REGS + 34
    for bank = 0, 7 do
        prog:write_u16(0xf9800, bank << 8)
        for i = 0, 0x3ff do prog:write_u16(0xf8800 + 2 * i, u16(s, P + 2 * (bank * 0x400 + i))) end
    end
    local S = P + 0x4000
    for i = 0, 0x3ff do prog:write_u16(0xf8000 + 2 * i, u16(s, S + 2 * i)) end
    prog:write_u16(0xf9800, vc)
    for l = 0, 2 do
        for w = 0, 3 do io_s:write_u16(0x80 + 8 * l + 2 * w, u16(s, REGS + 2 * (l * 4 + w))) end
        io_s:write_u16(0x98 + 2 * l, u16(s, REGS + 24 + 2 * l))
    end
end
emu.register_frame_done(function()
    n = n + 1
    if n == 2 then
        local rom = manager.machine.memory.regions[":maincpu"]
        rom:write_u8(0x7fff0, 0xeb); rom:write_u8(0x7fff1, 0xfe)   -- jmp $ with interrupts off
        manager.machine:soft_reset()
    elseif n > 10 then
        if stage == 0 then write_state(files[idx]); stage = 1
        elseif stage < 3 then stage = stage + 1
        else
            manager.machine.video:snapshot()
            print("SNAP", files[idx]); idx = idx + 1; stage = 0
            if idx > #files then manager.machine:exit() end
        end
    end
end)
