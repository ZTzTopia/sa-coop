// A race with the players who joined it (Race.h). CPRACE keeps its racers in arrays of its mission locals (the player among them at 442@) and
// its route in three more: what it compares is read from there.
#include "features/story/race/Race.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "common/api/protocol/Protocol.h"
#include "common/lib/log/Log.h"
#include "common/lib/util/Util.h"
#include "entities/mission/Mission.h"
#include "entities/player/Players.h"
#include "shared/api/net-session/Session.h"
#include "shared/config/Config.h"
#include "shared/game/Game.h"
#include "shared/game/Script.h"
#include "shared/lib/core/Core.h"
#include "shared/ui/chat/Chat.h"
#include "shared/ui/renderer/Renderer.h"

namespace coop::race {

namespace {
using namespace game;
using game::script::Cmd;

enum : uint8_t { OP_STATE = 1, OP_DONE = 2, OP_WATCH = 3, OP_START = 4, OP_READY = 5, OP_GO = 6, OP_PRIZE = 7 };
constexpr uintptr_t kMissionLocals = 0xA48960;  // CTheScripts::LocalVariablesForCurrentMission
// CPRACE's locals: its stage (7..10 the countdown, 11 and 12 the race, 13 its player finished), the laps, the route's
// checkpoints (a lap), the racers, the player's index among them, a checkpoint's radii, the race checkpoint's type and
// size; arrays of 16 racers: the car, the ped, the laps done, the next checkpoint and its place; the route: x, y, z
constexpr int kAhead = 229;  // the racers ahead of its player, as its screen shows the place (450@ = 229@ + 1)
constexpr int kStage = 49, kLaps = 224, kCheckpoints = 250, kRaceCheckpoint = 449, kPlace = 450, kRacers = 256, kPlayer = 442, kRadius = 322, kType = 205,
              kSize = 325, kRacerPed = 395, kRacerLaps = 273, kRacerNext = 257, kNextX = 331, kNextY = 347, kNextZ = 363,
              kRouteX = 454, kRouteY = 536, kRouteZ = 618, kRoute = 82;
// The grid: the race puts its cars on the places its mission set, facing the heading in $288. A rider takes a racer's
// place from the back; that racer's car and driver go first, so their copies are gone when the rider's car is made.
constexpr int kRacerCar = 379, kRacerModel = 716, kColor1 = 209, kColor2 = 215, kArea = 221;
constexpr uint32_t kGridHeading = 288, kCountdownAt = 27840, kTimer = 64;
constexpr uint64_t kStateMs = 250, kQuietMs = 3000, kReadyWaitMs = 8000, kGatherMs = 15000, kAfterMs = 180000;
constexpr uint32_t kPrize = 10000;  // what CPRACE pays its winner (the 8-Track, the Dirt Track and every Race Tournament race)

int32_t Int(int n) { return Ref<int32_t>(kMissionLocals + (uintptr_t)n * 4); }
float Float(int n) { return Ref<float>(kMissionLocals + (uintptr_t)n * 4); }
int32_t& Global(uint32_t offset) { return Ref<int32_t>(addr::ScriptSpace + offset); }
CVector Route(int i) { return {Float(kRouteX + i), Float(kRouteY + i), Float(kRouteZ + i)}; }

void* Cprace() {
    void* s = RunningMission();
    return s && !_strnicmp(Field<char[8]>(s, 0x08), "cprace", 8) ? s : nullptr;
}

struct Progress {
    bool done = false;
    int laps = 0, next = 0;
    float dist = 0;
};
// CPRACE's rule (its racers' loop at 0x10173D): who finished, then more laps, then a later checkpoint, then nearer to it
bool Ahead(const Progress& a, const Progress& b) {
    if (a.done != b.done) return a.done;
    if (a.laps != b.laps) return a.laps > b.laps;
    if (a.next != b.next) return a.next > b.next;
    return a.dist < b.dist;
}
bool g_taken[16] = {};  // the race's own racers a rider took the grid place of
bool RacerOf(int i, Progress& p) {
    if (g_taken[i]) return false;  // its car and driver are gone; the locals still hold its old handle
    CPed* ped = GetPed(Int(kRacerPed + i));
    if (!ped || ped->Health() <= 0.0f) return false;  // CPRACE leaves a dead one out too
    p.laps = Int(kRacerLaps + i), p.next = Int(kRacerNext + i);
    p.done = p.laps >= Int(kLaps) || Float(kNextZ + i) == 99999.0f;  // a finished one's mark
    p.dist = (ped->GetPosition() - CVector{Float(kNextX + i), Float(kNextY + i), Float(kNextZ + i)}).Length();
    return true;
}

// ---------------------------------------------------------------- the owner
struct Owner {
    Progress them;      // that player's run
    bool racing = false;
    bool told = false;  // its end said
    uint64_t sentMs = 0;
};
std::map<uint8_t, Owner> g_own;  // who joined our race

// The start: each rider's grid place, and whether that rider's car is already there. The race waits at "3" until they
// are all seated (or 8 s pass), then its own countdown runs.
struct Spot {
    CVector pos;
    float heading = 0;
};
struct Grid {
    std::map<uint8_t, Spot> spots;
    std::map<uint8_t, bool> ready;
    bool prepared = false, sent = false, holding = false;
    uint64_t preparedMs = 0;
    uint16_t model = 0, health = 0;
    uint8_t area = 0, col1 = 0, col2 = 0;
    int stage = 0;
};
Grid g_grid;
int g_removed = 0;         // the race's own racers whose grid places the riders took
uint64_t g_gatherMs = 0;   // when the call for this race opened the gather
int g_finished = 0;        // the riders that finished, including ones that left after it
void* g_script = nullptr;  // the CPRACE thread these counters belong to
bool g_teamWon = false;    // a rider finished first: the owner's racer is put first so its mission passes
int g_hostMoney = 0;       // the owner's money before its fake finish: only the prize it pays back is taken

// The finish window: when the race's own game ends (its player finished, failed or left the race), the riders still on
// the track keep their route and the places the race had at that moment, and ride for up to kAfterMs. CPRACE wipes its
// locals when its mission ends, so everything the tracking needs is copied while it still runs.
struct Frozen {
    bool valid = false;
    Progress p;
};
struct Window {
    bool on = false, story = false;
    uint64_t untilMs = 0;
    int count = 0, laps = 0, type = 0, me = -1, racers = 0;
    float radius[3] = {}, size = 0;
    CVector route[kRoute];
    Frozen ai[16];
};
Window g_win;
bool g_racing = false;  // our race had riders on the track

void SendPrize(uint8_t to, uint32_t amount) {
    Writer w(proto::G_RACE);
    w.U8(OP_PRIZE), w.U8(to), w.U32(amount);
    Net().Send(proto::CH_RELIABLE, w);
    LOGI("Race for two: prize $%u to %s", amount, Net().PeerName(to).c_str());
}

void SendDone(uint8_t with, Owner& o, uint8_t place, uint8_t racers, bool finished) {
    if (place < 1) place = 1;
    if (racers < 1) racers = 1;
    if (place > racers) place = racers;
    Writer w(proto::G_RACE);
    w.U8(OP_DONE), w.U8(with), w.U8(place), w.U8(racers), w.U8(finished ? 1 : 0);
    Net().Send(proto::CH_RELIABLE, w);
    o.told = true;
    LOGI("Race for two: %s %s, place %u of %u", Net().PeerName(with).c_str(), finished ? "finished" : "stopped", place, racers);
}

// the places now: that player's and ours among the racers and those who joined
void Places(const Owner& o, int& theirs, int& ours, int& racers) {
    int n = std::clamp(Int(kRacers), 0, 16), me = Int(kPlayer);
    Progress own;
    bool haveOwn = me >= 0 && me < n && RacerOf(me, own);
    theirs = ours = 1, racers = std::max(1, n - g_removed + (int)g_own.size());
    for (int i = 0; i < n; ++i) {
        Progress p;
        if (!RacerOf(i, p)) continue;
        if (Ahead(p, o.them)) ++theirs;
        if (i != me && haveOwn && Ahead(p, own)) ++ours;
    }
    for (auto& [id, other] : g_own) {
        if (&other != &o && Ahead(other.them, o.them)) ++theirs;
        if (haveOwn && Ahead(other.them, own)) ++ours;
    }
    theirs = std::min(theirs, racers);  // a removed racer's reused handle must not put a place past the field
    ours = std::min(ours, racers);
}

// The riders take the grid places of the race's own racers from the back, one each: a place on the grid is on the
// track and faces the race. Those racers go before the start is sent, so their copies are gone when the riders' cars
// are made there. Riders beyond the racers stand behind the grid.
void PrepareGrid(const std::vector<uint8_t>& riders) {
    int me = Int(kPlayer), racers = std::clamp(Int(kRacers), 1, 16);
    if (me < 0 || me >= racers) return;  // no player's racer: no place to take
    CVehicle* mine = GetVehicle(Int(kRacerCar + me));
    if (!mine) return;
    float gh = Ref<float>(addr::ScriptSpace + kGridHeading);  // the heading the mission set its grid to, in degrees
    CVector fwd{-std::sin(gh / 57.29578f), std::cos(gh / 57.29578f), 0};
    std::vector<std::pair<float, int>> bots;  // how far behind the owner, racer
    for (int i = 0; i < racers; ++i)
        if (i != me)
            if (CVehicle* v = GetVehicle(Int(kRacerCar + i)); v && GetPed(Int(kRacerPed + i)))
                bots.push_back({(v->GetPosition() - mine->GetPosition()).Dot(fwd), i});
    std::sort(bots.begin(), bots.end());
    CVector back = mine->GetPosition();
    for (int i = 0; i < racers; ++i)
        if (CVehicle* v = GetVehicle(Int(kRacerCar + i)); v && (v->GetPosition() - back).Dot(fwd) < 0) back = v->GetPosition();
    g_grid = {};
    g_removed = 0;
    std::fill(g_taken, g_taken + 16, false);
    g_grid.model = (uint16_t)Int(kRacerModel + me);
    g_grid.health = (uint16_t)std::clamp((int)mine->Health(), 1000, 65535);
    g_grid.area = (uint8_t)Int(kArea);
    g_grid.col1 = (uint8_t)Int(kColor1 + std::min(me, 5));
    g_grid.col2 = (uint8_t)Int(kColor2 + std::min(me, 5));
    size_t k = 0;
    for (uint8_t id : riders) {
        Spot spot;
        if (k < bots.size()) {
            int i = bots[k].second;
            if (CVehicle* v = GetVehicle(Int(kRacerCar + i))) {
                spot.pos = v->GetPosition();
                spot.heading = v->GetHeading() * 57.29578f;
                Cmd(0x009B).I(Int(kRacerPed + i)).Run();  // DELETE_CHAR
                Cmd(0x00A6).I(Int(kRacerCar + i)).Run();  // DELETE_CAR
                g_taken[i] = true;
                ++g_removed;
            }
        } else {
            float side = (k % 2) ? 2.5f : -2.5f;
            spot.pos = back - fwd * (8.0f + 4.0f * (float)(k - bots.size()));
            spot.pos.x += fwd.y * side;
            spot.pos.y -= fwd.x * side;
            spot.heading = gh;
        }
        g_grid.spots[id] = spot;
        g_grid.ready[id] = false;
        ++k;
    }
    g_grid.prepared = true;
    g_grid.preparedMs = NowMs();
    LOGI("Race for two: %zu grid places, %d racers taken", g_grid.spots.size(), g_removed);
}

void SendGrid(const std::vector<uint8_t>& riders) {
    for (uint8_t id : riders) {
        auto it = g_grid.spots.find(id);
        if (it == g_grid.spots.end()) continue;
        Writer w(proto::G_RACE);
        w.U8(OP_START), w.U8(id), w.U8(g_grid.area), w.U16(g_grid.model), w.U8(g_grid.col1), w.U8(g_grid.col2),
            w.U16(g_grid.health), w.Vec({it->second.pos.x, it->second.pos.y, it->second.pos.z}), w.F32(it->second.heading);
        Net().Send(proto::CH_RELIABLE, w);
    }
    g_grid.sent = true;
    g_grid.holding = true;
    LOGI("Race for two: grid start sent to %zu riders", riders.size());
}

bool AllReady(const std::vector<uint8_t>& riders) {
    for (uint8_t id : riders)
        if (g_grid.spots.count(id) && !g_grid.ready[id]) return false;
    return true;
}

void GridReset() {
    g_grid = {};
    g_removed = 0;
    g_gatherMs = 0;
    g_finished = 0;
    g_script = nullptr;
    g_teamWon = false;
    g_hostMoney = 0;
    std::fill(g_taken, g_taken + 16, false);
}

// ---------------------------------------------------------------- the finish window
void WindowSnapshot() {
    int count = Int(kCheckpoints), laps = Int(kLaps);
    if (count <= 0 || count > kRoute || laps <= 0) return;
    g_win.story = StoryRace();  // the window keeps the race's kind, even if another mission starts during it
    g_win.count = count, g_win.laps = laps, g_win.type = Int(kType), g_win.size = Float(kSize);
    for (int i = 0; i < 3; ++i) g_win.radius[i] = Float(kRadius + i);
    for (int i = 0; i < count; ++i) g_win.route[i] = Route(i);
    int n = std::clamp(Int(kRacers), 0, 16), joined = 0;
    for (auto& [id, o] : g_own) joined += o.racing ? 1 : 0;
    g_win.me = Int(kPlayer);
    g_win.racers = std::max(1, n - g_removed + joined);
    for (int i = 0; i < 16; ++i) {
        Progress p;
        g_win.ai[i].valid = i < n && RacerOf(i, p);
        if (g_win.ai[i].valid) g_win.ai[i].p = p;
    }
}

void WindowPlaces(const Owner& o, int& theirs, int& racers) {
    theirs = 1, racers = std::max(1, g_win.racers);
    for (int i = 0; i < 16; ++i)
        if (g_win.ai[i].valid && Ahead(g_win.ai[i].p, o.them)) ++theirs;
    for (auto& [id, other] : g_own)
        if (&other != &o && Ahead(other.them, o.them)) ++theirs;
    theirs = std::min(theirs, racers);
}

int WindowOwnPlace() {
    if (g_win.me < 0 || g_win.me >= 16 || !g_win.ai[g_win.me].valid) return 0;
    const Progress& own = g_win.ai[g_win.me].p;
    int place = 1;
    for (int i = 0; i < 16; ++i)
        if (i != g_win.me && g_win.ai[i].valid && Ahead(g_win.ai[i].p, own)) ++place;
    for (auto& [id, o] : g_own)
        if (Ahead(o.them, own)) ++place;
    return std::min(place, std::max(1, g_win.racers));
}

void WindowRun(uint8_t with, Owner& o) {
    const RemotePlayer* rp = Players::Instance().Get(with);
    if (!rp || !rp->ped) return;
    Progress& t = o.them;
    CVector at = rp->ped->GetPosition();
    if (!t.done && !o.told) {
        CVector d = at - g_win.route[t.next];
        if (std::fabs(d.x) < g_win.radius[0] && std::fabs(d.y) < g_win.radius[1] && std::fabs(d.z) < g_win.radius[2]) {
            if (++t.next >= g_win.count) t.next = 0, ++t.laps;
            if (t.laps >= g_win.laps) {
                int done = 1;
                for (int i = 0; i < 16; ++i)
                    if (g_win.ai[i].valid && g_win.ai[i].p.done) ++done;
                done += g_finished;
                t.done = true;
                ++g_finished;
                if (done == 1 && !g_win.story) SendPrize(with, kPrize);
                SendDone(with, o, (uint8_t)std::min(done, g_win.racers), (uint8_t)g_win.racers, true);
                return;
            }
        }
    }
    if (o.told || NowMs() - o.sentMs < kStateMs) return;
    o.sentMs = NowMs();
    t.dist = (at - g_win.route[t.next]).Length();
    int theirs, racers;
    WindowPlaces(o, theirs, racers);
    bool last = t.laps == g_win.laps - 1 && t.next == g_win.count - 1;
    CVector next = g_win.route[t.next], after = g_win.route[(t.next + 1) % g_win.count];
    Writer w(proto::G_RACE);
    w.U8(OP_STATE), w.U8(with), w.U8((uint8_t)theirs), w.U8((uint8_t)racers), w.U8((uint8_t)WindowOwnPlace());
    w.U8((uint8_t)std::min(t.laps + 1, g_win.laps)), w.U8((uint8_t)g_win.laps);
    w.Vec({next.x, next.y, next.z}), w.Vec({after.x, after.y, after.z});
    w.U8(last ? 1 : (uint8_t)std::clamp(g_win.type, 0, 8)), w.F32(last ? 6.0f : g_win.size);
    Net().Send(proto::CH_STATE, w);
}

void WindowEnd() {
    for (auto& [id, o] : g_own)
        if (!o.told) {
            int theirs, racers;
            WindowPlaces(o, theirs, racers);
            SendDone(id, o, (uint8_t)theirs, (uint8_t)racers, false);
        }
    g_own.clear();
    g_win = {};
    g_racing = false;
    GridReset();
}

void WindowUpdate() {
    bool waiting = false;
    for (auto it = g_own.begin(); it != g_own.end();) {
        Owner& o = it->second;
        if (!o.racing) {
            it = g_own.erase(it);
            continue;
        }
        if (o.told) {
            ++it;  // finished: its place stays for the riders after it
            continue;
        }
        waiting = true;
        const RemotePlayer* rp = Players::Instance().Get(it->first);
        if (!rp || !rp->ped || rp->ped->Health() <= 0.0f) {
            int theirs, racers;
            WindowPlaces(o, theirs, racers);
            SendDone(it->first, o, (uint8_t)theirs, (uint8_t)racers, false);
            it = g_own.erase(it);
            continue;
        }
        WindowRun(it->first, o);
        ++it;
    }
    if (!waiting || NowMs() >= g_win.untilMs) {
        if (waiting) WindowEnd();  // the time is up
        else {
            g_own.clear();
            g_win = {};
            g_racing = false;
            GridReset();
        }
    }
}

void OwnerRun(uint8_t with, Owner& o);

// Cesar's races are the story's: CESAR and BCESAR launch CPRACE as they launch his missions, so it ran as the host's
// mission, with nobody joined. The partner stood on foot behind the racers while only the host could take the
// checkpoints (#139). Everyone by the host at its countdown races too (the user's choice, 16.11); a rider finishing
// first makes the race the team's and the host's racer is marked finished, so the mission passes with that finish.
std::vector<uint8_t> StoryRacers(void* s) {
    std::vector<uint8_t> out;
    CPlayerPed* me = FindPlayerPed(0);
    if (!Net().IsHost() || !StoryMission(s) || !me) return out;
    bool countdown = Int(kStage) < 11;
    for (auto& [id, rp] : Players::Instance().All()) {
        bool near = rp.ped && rp.ped->AreaCode() == me->AreaCode() && (rp.ped->GetPosition() - me->GetPosition()).Length() < 150.0f;
        if (g_own.count(id) || (countdown && near)) out.push_back(id);
    }
    return out;
}

// The race's finish is checked against the player's place (229@): a host marked finished but still behind an AI racer
// is not the winner. Keep every other racer behind its player and clear that place as well.
void HoldTeamField() {
    int me = Int(kPlayer), n = std::clamp(Int(kRacers), 0, 16);
    if (me < 0 || me >= n) return;
    for (int i = 0; i < n; ++i)
        if (i != me) Ref<int32_t>(kMissionLocals + (uintptr_t)(kRacerLaps + i) * 4) = 0;
    Ref<int32_t>(kMissionLocals + (uintptr_t)kAhead * 4) = 0;
}

void OwnerUpdate() {
    void* s = Cprace();
    if (!s) g_script = nullptr;
    if (s && s != g_script) {  // a new race: none of the old one's grid, places or counters
        GridReset();
        g_script = s;
    }
    if (s && g_win.on) WindowEnd();  // a new race: the old one's window is over
    if (!s && g_win.on) {            // its mission ended: the riders still race on the route it had
        WindowUpdate();
        return;
    }
    if (!s && g_racing) {
        // A race of its own (the stadium, the track, the tournaments) or a story race: a rider finishing first ends
        // the owner's race too. Its racer is marked first so the race's own result passes; a side race's rider is
        // paid by OP_PRIZE. The owner's fake first place makes its own game pay a prize too: take that copy back.
        if (!g_win.story && g_teamWon) {
            int refund = std::max(0, PlayerInfos()[0].money - g_hostMoney);
            if (refund > 0) {
                PlayerInfos()[0].money -= refund;
                LOGI("Race for two: the host's fake race prize (-$%d) goes to the rider", refund);
            }
        }
        g_racing = false;
        if (!g_own.empty() && g_win.count > 0 && g_win.laps > 0) {
            g_win.on = true;
            g_win.untilMs = NowMs() + kAfterMs;
            LOGI("Race for two: our race is over, the riders still on it have %llu s", (unsigned long long)(kAfterMs / 1000));
            WindowUpdate();
            return;
        }
    }
    std::vector<uint8_t> helpers = s && SideLeading() ? SideHelpers() : s ? StoryRacers(s) : std::vector<uint8_t>{};
    for (auto it = g_own.begin(); it != g_own.end();) {  // its end, or that player left it
        bool inRace = std::find(helpers.begin(), helpers.end(), it->first) != helpers.end();
        if (SideLeading() && !(g_grid.sent && g_grid.ready[it->first])) inRace = false;  // no grid car: not a racer
        if (inRace) {
            ++it;
            continue;
        }
        if (it->second.racing && !it->second.told) {
            int theirs, ours, racers;
            Places(it->second, theirs, ours, racers);
            SendDone(it->first, it->second, (uint8_t)theirs, (uint8_t)racers, false);
        }
        it = g_own.erase(it);
    }
    for (uint8_t with : helpers) {
        if (SideLeading() && !(g_grid.sent && g_grid.ready[with])) continue;  // no grid car: not a racer
        if (!g_own.count(with)) LOGI("Race for two with %s", Net().PeerName(with).c_str());
        OwnerRun(with, g_own[with]);
    }
    if (s) {
        WindowSnapshot();
        int st = Int(kStage);
        if (g_teamWon && st >= 11 && st <= 12) HoldTeamField();  // the riders' race: its own racers stay behind
        if (st >= 11 && !g_own.empty()) g_racing = true;
    }
    int stage = s ? Int(kStage) : 0;
    if (s && stage >= 7 && stage < 11) {
        // The call is open for 15 s: the race stands at "3" until it ends or a partner joins, then the grid is set with
        // whoever is in it. Without a partner there is nothing to wait for.
        if (!g_gatherMs && SideOffering()) g_gatherMs = NowMs();
        bool gathering = g_gatherMs && SideOffering() && NowMs() - g_gatherMs < kGatherMs;
        if (gathering) {
            if (stage == 8) Global(kCountdownAt) = Global(kTimer) + 999;  // "$27840: the countdown's next step": at 3
        } else if (!g_grid.sent && SideLeading()) {
            if (!g_grid.prepared) PrepareGrid(helpers);
            else if (NowMs() - g_grid.preparedMs >= 300) SendGrid(helpers);  // the removed racers' copies go first
        }
    }
    if (s && g_grid.sent) {
        if (g_grid.holding) {
            if (AllReady(helpers) || NowMs() - g_grid.preparedMs >= kReadyWaitMs) {
                g_grid.holding = false;
                LOGI("Race for two: the countdown is released");
            } else if (stage == 8) {
                Global(kCountdownAt) = Global(kTimer) + 999;  // "$27840: the countdown's next step": keep it at 3
            }
        }
        if (stage >= 11 && g_grid.stage < 11) {
            for (uint8_t id : helpers) {
                Writer w(proto::G_RACE);
                w.U8(OP_GO), w.U8(id);
                Net().Send(proto::CH_RELIABLE, w);
            }
            ui::TheChat().AddSystem(TR("Гонка началась: вперёд!", "The race is on: go!"));
        }
        g_grid.stage = stage;
    } else if (!s && g_grid.prepared) {
        GridReset();
    }
}

void OwnerRun(uint8_t with, Owner& o) {
    const RemotePlayer* rp = Players::Instance().Get(with);
    if (!rp || !rp->ped) return;
    int stage = Int(kStage), count = Int(kCheckpoints), laps = Int(kLaps);
    if (stage < 7 || count <= 0 || count > kRoute || laps <= 0) return;
    Progress& t = o.them;
    CVector at = rp->ped->GetPosition();
    if (stage < 11) {
        t = {};  // the countdown: from the start line
    } else if (!t.done && !o.told) {
        o.racing = true;
        CVector next = Route(t.next), d = at - next;
        if (std::fabs(d.x) < Float(kRadius) && std::fabs(d.y) < Float(kRadius + 1) && std::fabs(d.z) < Float(kRadius + 2)) {
            if (++t.next >= count) t.next = 0, ++t.laps;  // as CPRACE does for its racers
            if (t.laps >= laps) {
                int done = 1, n = std::clamp(Int(kRacers), 0, 16);  // the ones that finished before it
                for (int i = 0; i < n; ++i)
                    if (Progress p; RacerOf(i, p) && p.done) ++done;
                done += g_finished;
                t.done = true;
                ++g_finished;
                if (done == 1) {
                    // A rider crossing first ends the owner's race too, in a stadium race as in a story race. Its
                    // racer is put first so the race shows its result; the rider is the real winner. A side race pays
                    // it, a story race has no G_RACE prize, and the owner's fake first place pays nothing.
                    g_teamWon = true;
                    int me = Int(kPlayer);
                    if (me >= 0 && me < 16) {
                        HoldTeamField();
                        Ref<int32_t>(kMissionLocals + (uintptr_t)(kRacerLaps + me) * 4) = Int(kLaps);
                        Ref<float>(kMissionLocals + (uintptr_t)(kNextZ + me) * 4) = 99999.0f;
                        // CPRACE checks a finish only after a racer crosses the last checkpoint; the place alone
                        // therefore never ends its race. Move its stage to the finish as its own finish would.
                        int checkpoint = Int(kRaceCheckpoint);
                        if (checkpoint >= 0) Cmd(0x06D6).I(checkpoint).Run();  // DELETE_CHECKPOINT
                        g_hostMoney = PlayerInfos()[0].money;  // before the game pays its prize for that finish
                        Ref<int32_t>(kMissionLocals + (uintptr_t)kPlace * 4) = 1;  // 450@: the place its stage 13 reads
                        Ref<int32_t>(kMissionLocals + (uintptr_t)kStage * 4) = 13;
                        LOGI("Race for two: its player's racer is marked finished first (the riders' race)");
                    }
                    if (!StoryRace()) SendPrize(with, kPrize);  // the rider is the real winner; the fake host win is not paid
                }
                SendDone(with, o, (uint8_t)done, (uint8_t)std::max(1, n - g_removed + (int)g_own.size()), true);
                return;
            }
        }
    }
    if (o.told || NowMs() - o.sentMs < kStateMs) return;
    o.sentMs = NowMs();
    t.dist = (at - Route(t.next)).Length();
    int theirs, ours, racers;
    Places(o, theirs, ours, racers);
    bool last = t.laps == laps - 1 && t.next == count - 1;  // its next one is the finish
    CVector next = Route(t.next), after = Route((t.next + 1) % count);
    Writer w(proto::G_RACE);
    w.U8(OP_STATE), w.U8(with), w.U8((uint8_t)theirs), w.U8((uint8_t)racers), w.U8((uint8_t)ours);
    w.U8((uint8_t)std::min(t.laps + 1, laps)), w.U8((uint8_t)laps);
    w.Vec({next.x, next.y, next.z}), w.Vec({after.x, after.y, after.z});
    w.U8(last ? 1 : (uint8_t)std::clamp(Int(kType), 0, 8)), w.F32(last ? 6.0f : Float(kSize));
    Net().Send(proto::CH_STATE, w);
}

// Our race for the other player riding with us or near us without racing itself: our place as our screen shows it,
// twice a second
uint64_t g_watchSentMs = 0;
void SendWatch() {
    void* s = Cprace();
    int stage = s ? Int(kStage) : 0, racers = s ? Int(kRacers) : 0;
    if (!s || stage < 11 || stage > 13 || racers < 2 || racers > 16 || NowMs() - g_watchSentMs < 500) return;
    g_watchSentMs = NowMs();
    Writer w(proto::G_RACE);
    w.U8(OP_WATCH), w.U8(0xFF), w.U8((uint8_t)std::clamp(Int(kAhead) + 1, 1, racers)), w.U8((uint8_t)racers);
    Net().Send(proto::CH_STATE, w);
}

// ---------------------------------------------------------------- the one who joined
struct Joined {
    uint8_t from = 0xFF;
    uint64_t heardMs = 0;
    int place = 0, racers = 0, ownerPlace = 0, lap = 0, laps = 0;
    CVector next;
    int checkpoint = -1, blip = -1;
    uint64_t carTryMs = 0;  // the host's story race: the last look for a car of our own (StoryCar)
} g_join;

// The host's story race (StoryRacers): on foot, or in the host's car, we get one like it beside us, as the one who joins a
// joint race gets one (BoardSide). One of our own we drive races as it is. Asked once a second while the host's car stands
// within 60 m, its start: CPRACE seats CJ a moment after it starts, and moves the car to its grid, which brings us over on
// foot (CS_FOLLOW, 16.11). Once the host drives off, one on foot watches.
void StoryCar() {
    if (g_join.from != Net().HostId() || _stricmp(HostMissionName().c_str(), "cprace") || NowMs() - g_join.carTryMs < 1000) return;
    g_join.carTryMs = NowMs();
    const RemotePlayer* host = Players::Instance().Get(Net().HostId());
    CPlayerPed* me = FindPlayerPed(0);
    if (!host || !host->ped || !me || me->Health() <= 0.0f || me->AttachedTo()) return;
    CVehicle* hv = host->ped->InVehicle() ? host->ped->Vehicle() : nullptr;
    if (!hv || (me->InVehicle() && me->Vehicle() != hv) || hv->MoveSpeed().Length() * 50.0f > 1.0f ||
        (hv->GetPosition() - me->GetPosition()).Length() > 60.0f)
        return;
    // the host's passenger, or right by its car: aside first, as a joint job's does (BoardSide). Put out where it sat, it
    // stood on the car's roof, and no spot beside it was in sight for a car (16.11). The side is the car's: its driver's
    // copy turned elsewhere put us 2.5 m ahead of it every second.
    if (me->InVehicle() || (hv->GetPosition() - me->GetPosition()).Length() < 3.0f) {
        CMatrix* m = hv->Matrix();
        float heading = m ? std::atan2(-m->forward.x, m->forward.y) : host->ped->CurrentRotation();
        CVector at = hv->GetPosition();
        at.z = host->ped->GetPosition().z;
        Players::Instance().GoTo(*host, at + PartnerSide(heading, Net().MyId(), 3.5f), true);
        LOGI("The host's story race: beside the host's car for one of our own");
        return;
    }
    Players::Instance().RideWith(*host, false);
}

// The rider's grid start: the car made at the place the owner sent, with the controls off until its go.
struct StartRide {
    bool on = false, go = false;
    uint8_t from = 0xFF;
    int car = -1, area = 0;
    uint64_t ms = 0;
} g_start;

void Unmark() {
    if (g_join.checkpoint != -1) Cmd(0x06D6).I(g_join.checkpoint).Run();  // DELETE_CHECKPOINT
    if (g_join.blip != -1) Cmd(0x0164).I(g_join.blip).Run();               // REMOVE_BLIP
    g_join.checkpoint = g_join.blip = -1;
}
void JoinEnd() {
    if (g_start.on && !g_start.go) Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL on
    g_start = {};
    if (g_join.from == 0xFF) return;
    Unmark();
    g_join = {};
}

// The owner put us on its grid: make its race's car there and sit in it until the countdown ends. The old car (the one
// we came with, a seat beside the owner) goes if it was ours.
void HandleStart(uint8_t from, Reader& r) {
    int area = r.U8(), model = r.U16(), col1 = r.U8(), col2 = r.U8(), health = r.U16();
    Vec3 p = r.Vec();
    float heading = r.F32();
    if (!r.Ok() || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(heading) ||
        !IsVehicleModel(model) || from != SideLeader())
        return;
    CPlayerPed* me = FindPlayerPed(0);
    if (!me || me->Health() <= 0.0f || !HasRoom(VehiclePool(), kKeepVehicles, "vehicle") || !LoadModelNow(model)) return;
    CVector spot{p.x, p.y, p.z};
    int handle = GetPedRef(me);
    bool wasInCar = me->InVehicle();
    int oldRef = -1;
    bool oldMine = false;
    if (CVehicle* old = me->VehicleIfIn()) oldRef = GetVehicleRef(old), oldMine = old->Driver() == me;
    if (wasInCar) Cmd(0x0362).I(handle).F(spot.x).F(spot.y).F(spot.z + 1.0f).Run();  // WARP_CHAR_FROM_CAR_TO_COORD
    Cmd(0x04BB).I(area).Run();                                                       // SET_AREA_VISIBLE
    Cmd(0x0860).I(handle).I(area).Run();                                             // SET_CHAR_AREA_VISIBLE
    if (!wasInCar) Cmd(0x00A1).I(handle).F(spot.x).F(spot.y).F(spot.z + 1.0f).Run();  // SET_CHAR_COORDINATES
    if (oldMine) Cmd(0x00A6).I(oldRef).Run();                                        // DELETE_CAR
    Cmd(0x04E4).F(spot.x).F(spot.y).Run();                                           // REQUEST_COLLISION
    Cmd(0x03CB).F(spot.x).F(spot.y).F(spot.z).Run();                                 // LOAD_SCENE
    LightArea(area, spot);
    float ground = -200.0f;
    Cmd(0x02CE).F(spot.x).F(spot.y).F(spot.z + 2.0f).Out(&ground).Run();  // GET_GROUND_Z_FOR_3D_COORD
    if (std::fabs(ground - spot.z) < 3.0f) spot.z = ground;
    int car = -1;
    Cmd(0x00A5).I(model).F(spot.x).F(spot.y).F(spot.z).Out(&car).Run();  // CREATE_CAR
    if (car < 0) {
        Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL on: no race for us
        ui::TheChat().AddError(TR("Не получилось выйти на старт.", "Could not get to the start."));
        return;
    }
    Cmd(0x0175).I(car).F(heading).Run();          // SET_CAR_HEADING
    Cmd(0x0840).I(car).I(area).Run();             // SET_VEHICLE_AREA_VISIBLE
    Cmd(0x0229).I(car).I(col1).I(col2).Run();     // CHANGE_CAR_COLOUR
    Cmd(0x0224).I(car).I(std::max(health, 1000)).Run();  // SET_CAR_HEALTH: as the owner's race car
    Cmd(0x036A).I(handle).I(car).Run();           // WARP_CHAR_INTO_CAR
    Cmd(0x01B4).I(0).I(0).Run();                  // SET_PLAYER_CONTROL off until the go
    Cmd(0x0373).Run();                            // SET_CAMERA_BEHIND_PLAYER
    Cmd(0x02EB).Run();                            // RESTORE_CAMERA_JUMPCUT
    g_start = {true, false, from, car, area, NowMs()};
    Writer w(proto::G_RACE);
    w.U8(OP_READY), w.U8(from);
    Net().Send(proto::CH_RELIABLE, w);
    ui::TheChat().AddSystem(Format(TR("Старт со стартовой решётки игрока %s.", "Starting from %s's grid."),
                                   Net().PeerName(from).c_str()));
    LOGI("Race for two: on %s's grid, car %d at %.1f %.1f %.1f, heading %.0f", Net().PeerName(from).c_str(), car, spot.x,
         spot.y, spot.z, heading);
}

void HandleGo(uint8_t from) {
    if (!g_start.on || from != g_start.from || g_start.go) return;
    g_start.go = true;
    Cmd(0x01B4).I(0).I(1).Run();  // SET_PLAYER_CONTROL on
    ui::TheChat().AddSystem(TR("Вперёд!", "Go!"));
}

void ReadState(uint8_t from, Reader& r) {
    int place = r.U8(), racers = r.U8(), ownerPlace = r.U8(), lap = r.U8(), laps = r.U8();
    Vec3 n = r.Vec(), a = r.Vec();
    int type = r.U8();
    float size = r.F32();
    if (!r.Ok() || !std::isfinite(n.x) || !std::isfinite(n.y) || !std::isfinite(n.z) || !std::isfinite(a.x) || !std::isfinite(a.y) ||
        !std::isfinite(a.z) || !std::isfinite(size) || size <= 0.0f || size > 50.0f || type > 8 || racers > 19)
        return;
    if (g_join.from != from) {
        // An unreliable state can overtake the reliable start: it must not end the frozen grid car it is racing in.
        if (!(g_start.on && !g_start.go && from == g_start.from)) JoinEnd();
        g_join.from = from;
        ui::TheChat().AddSystem(TR("Гонка вдвоём: вы тоже соревнуетесь, место видно справа.",
                                   "A race for two: you race too, your place shows on the right."));
        LOGI("Race for two at %s's race", Net().PeerName(from).c_str());
    }
    g_join.heardMs = NowMs();
    g_join.place = place, g_join.racers = racers, g_join.ownerPlace = ownerPlace, g_join.lap = lap, g_join.laps = laps;
    CVector next{n.x, n.y, n.z};
    if (g_join.checkpoint != -1 && (next - g_join.next).Length() < 0.5f) return;
    Unmark();  // our next checkpoint moved on
    g_join.next = next;
    Cmd(0x06D5).I(type).F(n.x).F(n.y).F(n.z).F(a.x).F(a.y).F(a.z).F(size).Out(&g_join.checkpoint).Run();  // CREATE_CHECKPOINT
    Cmd(0x018A).F(n.x).F(n.y).F(n.z).Out(&g_join.blip).Run();  // ADD_BLIP_FOR_COORD
}

// "2-е место из 5": Russian puts the ending after the number
std::string Place(int place, int of) { return Format(TR("%d-е место из %d", "place %d of %d"), place, of); }

// ---------------------------------------------------------------- one riding with the racer or near it
struct Watch {
    uint8_t from = 0xFF;
    uint64_t heardMs = 0;
    int place = 0, racers = 0;
} g_watch;
void ReadWatch(uint8_t from, Reader& r) {
    int place = r.U8(), racers = r.U8();
    if (!r.Ok() || racers < 2 || racers > 16 || place < 1 || place > racers || g_join.from == from) return;  // racing ourselves
    const RemotePlayer* rp = Players::Instance().Get(from);
    CPlayerPed* me = FindPlayerPed(0);
    if (!rp || !rp->ped || !me || rp->ped->AreaCode() != me->AreaCode() || (rp->ped->GetPosition() - me->GetPosition()).Length() > 150.0f)
        return;  // elsewhere: its race is not ours to watch
    if (g_watch.from != from) LOGI("Watching %s's race", Net().PeerName(from).c_str());
    g_watch = {from, NowMs(), place, racers};
}

void Handle(uint8_t from, Reader& r) {
    uint8_t op = r.U8(), to = r.U8();
    if (!r.Ok() || (to != Net().MyId() && to != 0xFF) || from == Net().MyId() || !core::IsPlayerReady()) return;
    if (op == OP_WATCH) return ReadWatch(from, r);
    if (to != Net().MyId()) return;
    if (op == OP_START) return HandleStart(from, r);
    if (op == OP_GO) return HandleGo(from);
    if (op == OP_READY) {
        if (SideLeading() && g_grid.spots.count(from)) {
            g_grid.ready[from] = true;
            if (Int(kStage) >= 11) {  // the countdown went on without it: it is seated after the start
                Writer w(proto::G_RACE);
                w.U8(OP_GO), w.U8(from);
                Net().Send(proto::CH_RELIABLE, w);
            }
        }
        return;
    }
    if (op == OP_STATE) return ReadState(from, r);
    if (op == OP_PRIZE) {
        uint32_t amount = r.U32();
        if (!r.Ok() || from != g_join.from || amount == 0 || amount > 10000000) return;
        Cmd(0x0109).I(0).I((int)amount).Run();  // ADD_SCORE: the race's prize, once
        ui::TheChat().AddSystem(Format(TR("Победа в гонке! Приз: $%u.", "The race's prize: $%u."), amount));
        LOGI("Race for two: prize $%u", amount);
        return;
    }
    if (op != OP_DONE || from != g_join.from) return;
    int place = r.U8(), racers = r.U8();
    bool finished = r.U8() != 0;
    if (!r.Ok()) return;
    ui::TheChat().AddSystem(Format(finished ? TR("Финиш: %s.", "Finish: %s.") : TR("Гонка окончена: %s.", "The race is over: %s."),
                                   Place(place, racers).c_str()));
    LOGI("Race for two: %s, place %d of %d", finished ? "finished" : "over", place, racers);
    JoinEnd();
}

class RaceModule : public Module {
public:
    const char* Name() const override { return "Race"; }
    void OnInstallHooks() override { Net().SetHandler(proto::G_RACE, &Handle); }
    void OnSessionEnd() override { JoinEnd(), g_own.clear(), g_win = {}, g_racing = false, GridReset(); }
    void OnGameShutdown() override { g_join = {}, g_start = {}, g_own.clear(), g_win = {}, g_racing = false, GridReset(); }
    void OnPostProcess() override {
        if (!Net().Online() || !core::IsPlayerReady()) return;
        OwnerUpdate();
        SendWatch();
        StoryCar();
        if (g_join.from != 0xFF && NowMs() - g_join.heardMs > kQuietMs) JoinEnd();  // its race is over there
        if (g_start.on && !g_start.go && NowMs() - g_start.ms > kReadyWaitMs + 12000) {  // the start never came
            Cmd(0x01B4).I(0).I(1).Run();
            g_start = {};
        }
        if (g_watch.from != 0xFF && NowMs() - g_watch.heardMs > 2000) g_watch = {};
    }
    void OnRender() override {
        if (!core::IsPlayerReady() || MenuActive()) return;
        auto& g = ui::Gfx();
        float s = g.Scale(), x = g.Width() - 48 * s, y = g.Height() * 0.34f;
        if (g_join.from == 0xFF && g_watch.from != 0xFF) {  // the place of the one we ride with
            g.Text(ui::FONT_MENU_SMALL, x, y, Format(TR("Место игрока %s", "%s's place"), Net().PeerName(g_watch.from).c_str()),
                   0xFFE2C063, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_MENU_SMALL);
            g.GameText(ui::GAME_FONT_PRICEDOWN, x, y, Format("%d/%d", g_watch.place, g_watch.racers), 52 * s, 0xFFFFFFFF, ui::TEXT_RIGHT);
            return;
        }
        if (g_join.from == 0xFF && !g_own.empty() && Cprace()) {  // host: the joined players' places, as they see ours
            int theirs = 0, ours = 0, racers = 0;
            Places(g_own.begin()->second, theirs, ours, racers);
            g.Text(ui::FONT_MENU_SMALL, x, y, TR("Игроки", "Players"), 0xFFE2C063, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_MENU_SMALL);
            for (auto& [id, o] : g_own) {
                Places(o, theirs, ours, racers);
                g.Text(ui::FONT_SMALL, x, y, Format("%s: %d/%d", Net().PeerName(id).c_str(), theirs, racers), 0xFFFFFFFF,
                       ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
                y += g.LineHeight(ui::FONT_SMALL);
            }
            if (ours > 0)
                g.Text(ui::FONT_SMALL, x, y, Format(TR("Вы: %d/%d", "You: %d/%d"), ours, racers), 0xFFB4C8DC,
                       ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
            return;
        }
        if (g_join.from == 0xFF || !g_join.racers) return;
        g.Text(ui::FONT_MENU_SMALL, x, y, TR("Место", "Place"), 0xFFE2C063, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
        y += g.LineHeight(ui::FONT_MENU_SMALL);
        g.GameText(ui::GAME_FONT_PRICEDOWN, x, y, Format("%d/%d", g_join.place, g_join.racers), 52 * s, 0xFFFFFFFF, ui::TEXT_RIGHT);
        y += 56 * s;
        if (g_join.laps > 1) {
            g.Text(ui::FONT_SMALL, x, y, Format(TR("Круг %d из %d", "Lap %d of %d"), g_join.lap, g_join.laps), 0xFFFFFFFF,
                   ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
            y += g.LineHeight(ui::FONT_SMALL);
        }
        if (g_join.ownerPlace > 0)
            g.Text(ui::FONT_SMALL, x, y,
                   Format("%s: %d/%d", Net().PeerName(g_join.from).c_str(), g_join.ownerPlace, g_join.racers),
                   0xFFB4C8DC, ui::TEXT_RIGHT | ui::TEXT_OUTLINE);
    }

    static RaceModule& Instance() {
        static RaceModule m;
        return m;
    }
};

struct Reg {
    Reg() { core::RegisterModule(&RaceModule::Instance()); }
} g_reg;
}  // namespace

bool ForTwo() { return !g_own.empty() && Cprace(); }
bool WindowActive() { return g_win.on; }
bool GridPrepared() { return g_grid.prepared; }
bool InRace() { return g_join.from != 0xFF || g_start.on; }

}  // namespace coop::race
