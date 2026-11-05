' Walk around a room: the arrow keys move the ball, B stops the program.
' A 40 x 30 map of 8 x 8 tiles with a wall around it; the camera follows the ball.

gfx(8)
palette("1", 120, 80, 40)           ' wall
palette("2", 40, 120, 60)           ' floor
palette("3", 70, 150, 80)           ' a lighter spot on the floor
palette("4", 250, 210, 40)          ' the ball

for y = 0 to 7
  tilerow(1, y, "11111111")       ' tile 1: wall
  tilerow(2, y, "22222222")       ' tile 2: floor
  tilerow(3, y, "22222222")       ' tile 3: floor with a spot
next
tilerow(3, 3, "22233222")
tilerow(3, 4, "22233222")

sprrow(1, 0, "..4444..")          ' sprite 1: the ball
sprrow(1, 1, ".444444.")
for y = 2 to 5
  sprrow(1, y, "44444444")
next
sprrow(1, 6, ".444444.")
sprrow(1, 7, "..4444..")

newmap(40, 30)
tilefill(0, 0, 40, 30, 1)
tilefill(1, 1, 38, 28, 2)
for i = 1 to 40
  tile(1 + i * 7 mod 38, 1 + i * 11 mod 28, 3)
next
settileflags(1, 1)                ' walls are solid

p = actor(1, 160, 120)

while btn(0, "b") = 0
  dx = 0
  dy = 0
  if btn(0, "left") then dx = -2
  if btn(0, "right") then dx = 2
  if btn(0, "up") then dy = -2
  if btn(0, "down") then dy = 2
  h = actormove(p, dx, dy)
  camera(actorx(p) - 160, actory(p) - 120)
  waitframe()
wend

print actorx(p);
print actory(p);
