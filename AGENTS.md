# Repository Guidelines & Workflow

## Git & Branching Strategy
- **No Direct Commits to Main**: Do not commit or push directly to the `main` branch.
- **Dev Branch Workflow**: Always commit changes to the `dev` branch (or a dedicated feature branch off `dev`), then push to `origin` so a Pull Request can be created and reviewed in GitHub before merging into `main`.

## Verification & Testing Policy
- **Mandatory Pre-Commit Test Verification**: Always build and run the unit test suite (`MandelbrotViewer.Tests.exe`) before committing and pushing any changes. Every test must pass with 0 failures. Never commit or push changes if any unit test fails.
