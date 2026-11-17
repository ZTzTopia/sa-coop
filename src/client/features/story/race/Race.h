#pragma once
// A race for two (0.3.3). A joint race (CPRACE called in with Y, the story's races among them) runs in its owner's game,
// and the one who joined drove its own car beside the racers but did not race. Now the owner's game takes the other
// player for one more racer: it follows that player's copy through the race's route (CPRACE's checkpoints, laps and
// radii), ranks it with CPRACE's own rule (laps, then checkpoints, then the distance to the next one), and sends it its
// place, the owner's place and its own next checkpoint. The joined game shows them instead of the owner's checkpoint.
// One who did not join but rides with the racer or stays near it sees the racer's place.

namespace coop::race {

// owner: our race runs with the other player racing in it (its checkpoint and route marks are not sent: it has its own)
bool ForTwo();
// owner: the race ended, the riders still on its route race for up to three minutes
bool WindowActive();
// owner: the grid start is being prepared; a new rider can no longer take a place
bool GridPrepared();
// rider: we are on another player's grid or in its race
bool InRace();

}  // namespace coop::race
