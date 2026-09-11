# Repository workflow

- Commit completed, validated changes in focused local stages. The user requested
  ongoing commits; do not ask again before ordinary local commits. Do not push
  unless requested.
- Format owned C sources/headers with clang-format using .clang-format.
- Use tools/check_code.py with the appropriate compilation database for Clang
  diagnostics. Run tests appropriate to the change and update PLAN.md and docs
  when implementation scope changes.
- See docs/development.md for build and lint commands. Never commit build output
  or external dependency checkouts.
