-- Walk around a room: the arrow keys move the ball, B stops the program.
-- A 40 x 30 map of 8 x 8 tiles with a wall around it; the camera follows the ball.

gfx(8)
palette("1", 120, 80, 40)           -- wall
palette("2", 40, 120, 60)           -- floor
palette("3", 70, 150, 80)           -- a lighter spot on the floor
palette("4", 250, 210, 40)          -- the ball

for y = 0, 7 do
  tilerow(1, y, "11111111")         -- tile 1: wall
  tilerow(2, y, "22222222")         -- tile 2: floor
  tilerow(3, y, "22222222")         -- tile 3: floor with a spot
end
tilerow(3, 3, "22233222")
tilerow(3, 4, "22233222")

sprrow(1, 0, "..4444..")            -- sprite 1: the ball
sprrow(1, 1, ".444444.")
for y = 2, 5 do
  sprrow(1, y, "44444444")
end
sprrow(1, 6, ".444444.")
sprrow(1, 7, "..4444..")

newmap(40, 30)
tilefill(0, 0, 40, 30, 1)
tilefill(1, 1, 38, 28, 2)
for i = 1, 40 do
  tile(1 + i * 7 % 38, 1 + i * 11 % 28, 3)
end
settileflags(1, 1)                  -- walls are solid

local p = actor(1, 160, 120)

while not btn(0, "b") do
  local dx, dy = 0, 0
  if btn(0, "left") then dx = -2 end
  if btn(0, "right") then dx = 2 end
  if btn(0, "up") then dy = -2 end
  if btn(0, "down") then dy = 2 end
  actormove(p, dx, dy)
  camera(actorx(p) - 160, actory(p) - 120)
  waitframe()
end

print(actorx(p))
print(actory(p))
