# BluePaws V4 Working Instructions

## Canonical repository

The canonical GitHub repository is:

https://github.com/rees3901/BluepawsV4

The active Windows working directory depends on the host:

- `reesMiniPC`: `C:\Users\reesMiniPC\Desktop\cat tracker workspaces\BluePawsV4\BluepawsV4-git`
- `reesPC`: `C:\Users\reesPC\Desktop\Bluepaws V4`

All BluePaws firmware, PCB, documentation, simulator and application work must be
performed in the active host's Git checkout or a dedicated worktree created from it.

## Obsolete directories

Do not modify or treat these as active working copies:

- C:\Users\reesMiniPC\Desktop\cat tracker workspaces\BluePawsV4\BluepawsV4
- C:\Users\reesMiniPC\Desktop\cat tracker workspaces\BluePawsV4\PCB design

The first is an obsolete duplicate clone. The second is a temporary backup of the original PCB workspace.

## Repository verification

Before making changes, verify:

1. `git rev-parse --show-toplevel` points to the active host's checkout or the
   dedicated worktree for the task.
2. `git remote get-url origin` returns `https://github.com/rees3901/BluepawsV4.git`.
3. The current branch and working tree are checked with `git status --short --branch`.
4. The latest remote state has been fetched with `git fetch --prune origin`.

Do not initialise another Git repository inside this repository or any of its subdirectories.

## Working practice

- Start new work from an updated `main` branch.
- Create a dedicated feature or fix branch.
- Keep KiCad work under `pcb/`.
- Keep collar firmware under `collar/`.
- Keep hub firmware under `hub/`.
- Review staged files before committing.
- Push the branch and merge through a pull request.

## Walter and GM02SP decisions

Before changing GM02SP integration, GNSS or LTE behaviour, antennas, modem power,
or the collar PCB around these parts, read
`docs/firmware/collar/WALTER_GM02SP_UPSTREAM_REFERENCE.md`. It identifies the
relevant QuickSpot/Sequans sources and distinguishes Walter board properties
from requirements for a bare GM02SP on the production PCB. Check the current
vendor source and installed modem firmware before treating a value or command
as a design requirement.
