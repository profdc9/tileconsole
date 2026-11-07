' Self-checking test of the tile flags, the recorded hit tiles and the last position.
' Every line says ok or FAIL (with what it got and what it wanted); no graphics are shown.

gfx(8)
newmap(20, 12)

def chk(n$, got, want)
  if got = want then
    print "ok   " + n$;
  else
    print "FAIL " + n$ + " got " + str(got) + " want " + str(want);
  endif
enddef

' tile 1 solid, 2 one-way, 3 trigger, 4 solid + trigger
settileflags(1, 1)
settileflags(2, 2)
settileflags(3, 4)
settileflags(4, 5)
chk("flags solid", tileflag(0, 0), 0)

' a solid tile stops the actor flush against it and records what it was
tile(5, 2, 1)
a = actor(1, 16, 16)
r = actormove(a, 40, 0)
chk("solid: sides", r, 2)
chk("solid: x flush", actorx(a), 32)
chk("solid: hitx", hitx(a, "right"), 5)
chk("solid: hity", hity(a, "right"), 2)
chk("solid: hittile", hittile(a, 2), 1)
chk("solid: hitflags", hitflags(a, 2), 1)
tile(5, 2, 0)
chk("solid: kept after the map changed", hittile(a, "right"), 1)
chk("solid: nothing recorded on the left", hittile(a, "left"), 0)

' one-way: stops an actor coming down onto it, lets one go up through it
tile(2, 6, 2)
b = actor(1, 16, 30)
r = actormove(b, 0, 40)
chk("one-way down: sides", r, 8)
chk("one-way down: y flush", actory(b), 40)
chk("one-way down: flags", hitflags(b, "down"), 2)
c = actor(1, 16, 60)
r = actormove(c, 0, -30)
chk("one-way up: passes", r, 0)
chk("one-way up: y", actory(c), 30)

' trigger: reported, does not stop, and the last position is from before the touch
tile(8, 2, 3)
chk("tileflag trigger", tileflag(66, 20), 4)
chk("tilebox", tilebox(60, 16, 10, 8), 4)
d = actor(1, 56, 16)
r = actormove(d, 20, 0)
chk("trigger: reported", r, 16)
chk("trigger: not stopped", actorx(d), 76)
chk("trigger: tile x", hitx(d, "touch"), 8)
chk("trigger: tile y", hity(d, "touch"), 2)
chk("trigger: tile number", hittile(d, 16), 3)
chk("trigger: flags", hitflags(d, "touch"), 4)
chk("last x", lastx(d), 56)
chk("last y", lasty(d), 16)
actorpos(d, lastx(d), lasty(d))
chk("put back", actorx(d), 56)
chk("clear of it", actormove(d, 0, 0), 0)
chk("last unchanged by a move to the same place", lastx(d), 76)

' a tile that is solid and a trigger stops the actor like any solid one; the actor is flush against it, not overlapping it, so it does not count as touched
tile(8, 5, 4)
e = actor(1, 56, 40)
r = actormove(e, 30, 0)
chk("both: sides", r, 2)
chk("both: x flush", actorx(e), 56)
chk("both: right flags", hitflags(e, "right"), 5)
chk("both: not touched", hitflags(e, "touch"), 0)

print "done";
