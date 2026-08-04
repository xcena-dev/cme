# docs/ — which document answers which question

Each row below answers a different question.
Reading them in that order costs the least backtracking.

| | question | |
|---|---|---|
| [`articles/`](articles/) | Why does this exist? | The article draft. Argues the problem before the solution, for a reader who has not decided the problem is real. |
| [`../README.md`](../README.md) | What is it? | The project README: the assumption, the API, the shape, the failure model. |
| [`design/technical_report.md`](design/technical_report.md) | How does it work, and how well? | The design record. The reasoning behind each choice, and Section 11 measures what the choices cost. |
| [`spec/`](spec/) | What must always be true? | The TLA+ model and its configurations. Normative: `src/core/` implements this. |

## Which one to change

`spec/` is normative, so an invariant changes there first and the implementation follows.

`design/technical_report.md` is the source the articles derive from, as Section 0 of that document states.
A number or a claim therefore changes in the design record, and the article draft is brought back into line afterwards.

Figures live beside the document that reads them, in `design/figs/` and `articles/figs/`.
The project README reuses two of the article figures rather than keeping its own copies.
