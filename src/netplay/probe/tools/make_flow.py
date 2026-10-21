"""N Guest Play consoles: group, characters, camp, then the first Holo-Mission (frame script).
usage: make_flow.py N OUT"""
import sys

N = int(sys.argv[1])
FPS = 60
events = []
clock = [0] * N
CHAR_MOVES = {1: "", 2: "RIGHT", 3: "RIGHT RIGHT"}

def taps(c, seq, pause):
    for key in seq.split():
        events.append((clock[c], c, key))
        clock[c] += int(pause * FPS)

def sync(c, t):
    clock[c] = max(clock[c], t)

for c in range(N):
    clock[c] = 1200
    taps(c, "DOWN A", 1.2); clock[c] += 90
    taps(c, "DOWN A", 1.2); clock[c] += 120
    taps(c, "DOWN DOWN DOWN A", 0.8); clock[c] += 90
    taps(c, "LEFT A", 0.8); clock[c] += 150
    taps(c, "LEFT A", 0.8)
t = max(clock) + 4 * FPS
marks = {}
clock[0] = t; taps(0, "A", 4)                         # host: Create a Group
for g in range(1, N):
    sync(g, clock[0]); taps(g, "UP A", 0.9); clock[g] += 4 * FPS
    clock[0] = clock[g]                               # next guest after this one joined
t = max(clock)
marks["group"] = t - FPS
clock[0] = t; taps(0, "A", 3)                         # host: Proceed
t = clock[0]
for g in range(1, N):                                 # guests: own character, confirm
    sync(g, t)
    if CHAR_MOVES[g]: taps(g, CHAR_MOVES[g], 0.6)
    taps(g, "A", 1.5); t = clock[g]
sync(0, t); taps(0, "A", 2); t = clock[0]
for g in range(1, N):
    sync(g, t); taps(g, "A", 1.5); t = clock[g]
sync(0, t); taps(0, "A", 3)
marks["chars"] = clock[0] - FPS
taps(0, "START", 8)                                   # host: to the camp
marks["camp"] = clock[0] - FPS
taps(0, "A", 3)                                       # Holo-Missions
taps(0, "A", 2.5)                                     # the first mission
taps(0, "A", 2)                                       # Embark
taps(0, "LEFT A", 0.8); clock[0] += 3 * FPS           # Yes
t = clock[0]
for g in range(1, N):
    sync(g, t); taps(g, "START", 1)                   # guests: ready
t = max(clock)
sync(0, t); taps(0, "START", 3); taps(0, "A", 1)      # host: start
for k in range(12):                                   # through the briefing
    for c in range(N):
        sync(c, clock[0]); taps(c, "A", 0)
    clock[0] += int(1.5 * FPS)
marks["field"] = clock[0] + 20 * FPS
end = marks["field"] + 30 * FPS                       # 30 s in the field
with open(sys.argv[2], "w") as f:
    for frame, c, key in sorted(events):
        f.write(f"{frame} {c} {key} 6\n")
print(f"frames {end} marks {marks}")
