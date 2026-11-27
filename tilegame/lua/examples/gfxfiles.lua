-- Graphics and map files: draws some tiles and a map, saves them to the card (savegfx, savemap), starts the graphics afresh
-- (everything black), loads the files back (loadgfx, loadmap) and shows what came back. B stops the program.

gfx(8)
palette("1", 40, 60, 160)           -- sky
palette("2", 250, 220, 60)          -- a star
palette("3", 90, 160, 70)           -- grass
palette("4", 130, 90, 50)           -- earth
tiledef(1, string.rep("1", 64))
tiledef(2, string.rep("1", 27) .. "22" .. string.rep("1", 6) .. "22" .. string.rep("1", 27))
tiledef(3, string.rep("3", 16) .. string.rep("4", 48))
tiledef(4, string.rep("4", 64))
newmap(40, 30)
tilefill(0, 0, 40, 30, 1)
for i = 0, 39 do tile(i, 20, 3) end
tilefill(0, 21, 40, 9, 4)
for i = 1, 25 do tile((i * 7) % 40, (i * 5) % 18, 2) end
savegfx("gfxfiles.tgf")
savemap("gfxfiles.map")

gfx(8)                              -- afresh: no tiles, no map, every colour but the 16 black
loadgfx("gfxfiles.tgf")
loadmap("gfxfiles.map")
print("Loaded gfxfiles.tgf and gfxfiles.map back; B stops.")
local x = 0
while not pressed(0, "b") do
  x = x + 1
  camera(x % 320, 0)                -- (the map is 320 pixels wide: it shows how much of it came back)
  waitframe()
end
