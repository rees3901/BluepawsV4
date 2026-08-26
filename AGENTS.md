# BluePaws V4 Working Instructions

## Canonical repository

The canonical GitHub repository is:

https://github.com/rees3901/BluepawsV4

The canonical Windows working directory is:

C:\Users\reesMiniPC\Desktop\cat tracker workspaces\BluePawsV4\BluepawsV4-git

All BluePaws firmware, PCB, documentation, simulator and application work must be performed within this Git working directory.

## Obsolete directories

Do not modify or treat these as active working copies:

- C:\Users\reesMiniPC\Desktop\cat tracker workspaces\BluePawsV4\BluepawsV4
- C:\Users\reesMiniPC\Desktop\cat tracker workspaces\BluePawsV4\PCB design

The first is an obsolete duplicate clone. The second is a temporary backup of the original PCB workspace.

## Repository verification

Before making changes, verify:

1. `git rev-parse --show-toplevel` points to `BluepawsV4-git`.
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
