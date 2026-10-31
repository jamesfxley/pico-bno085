# pico-bno085 documentation

| Document | What it is | Read it when |
|---|---|---|
| [traceability-exceptions.txt](traceability-exceptions.txt) | Requirements no source file can cite (EN-02.7). | Reviewing uncitable requirements. |
| [orientation.md](orientation.md) | How this repository is built and gated, how it is laid out, and the repository's invariants and traps | Working in it for the first time, or before an edit |
| [technical-specification.md](technical-specification.md) | **Normative.** Requirement IDs (`AR`/`HR`/`PR`/`BR`/`TR`/`OR`), their pass criteria, and the traceability matrix | Deciding whether a behaviour is required or incidental, or changing one |
| [architecture.md](architecture.md) | Descriptive. How the implementation is put together — the driver façade, the HAL split, the transports | Working out where a change belongs |
| [usage-guide.md](usage-guide.md) | Practical use: wiring, bus selection, first run on a board | Bringing a part up for the first time |
| [future-work.md](future-work.md) | What is deliberately not built, and why | Asking why something is absent, or proposing it |
| [deviations.md](deviations.md) | Standing (scope-wide) deviations from the coding standard, each with a rationale, a risk assessment and a review date | Adding or reviewing a suppression |
| [deviation-budget.txt](deviation-budget.txt) | One suppression ceiling per language, ratcheting down. These only ever ratchet down | Same |
| [requirement-prefixes.txt](requirement-prefixes.txt) | The pinned prefix set, checked by review (EN-02.4) | Adding a requirement under a new prefix |
| [external-citations.txt](external-citations.txt) | Requirement IDs this repository cites from a sibling, each with the repository that defines it | Citing another repository's requirement, or auditing one |
| [Doxyfile](Doxyfile) | The API reference's configuration; the reference itself is generated into `api/` | Reading the public API, or checking a new entity is documented |
| [GitHub Releases](https://github.com/jamesfxley/pico-bno085/releases) | What changed in each release, and what to change in your code when an API broke | Upgrading across a release |

The specification is the only normative document here. `architecture.md`
describes what is, and work deliberately not done is an entry in
[future-work.md](future-work.md) (`EN-03.7`); the issue tracker holds defects.

A behaviour claimed in `README.md` or `docs/orientation.md` but absent from the
specification is not a commitment, and a requirement is not satisfied until the
matrix names what establishes it.

Measured results are not in this directory. Device runs and the conditions they
were captured under are the run record, and live with the benchmark:
[examples/bench/baselines/](../examples/bench/baselines/).

## The datasheet

Every electrical and timing claim here is confirmed against the part's
datasheet — CEVA 1000-3927, *BNO08X Datasheet*, v1.17 — not against any
open-source driver. It is not redistributed here, so citations name the document
and section and a reader obtains it from the vendor.

The SH-2 and SHTP protocol manuals are cited the same way by pico-sh2, which
owns the protocol layer.

## The API reference

The API reference is generated from the headers rather than written by hand, so
it cannot drift from the declarations it documents:

```sh
doxygen docs/Doxyfile      # -> docs/api/html/index.html (gitignored)
```

CI builds it on every push to `main` and every pull request, fails on an
undocumented public entity.

## Conventions

- Requirement IDs are stable and never renumbered. Code and tests cite the ID,
  never a section number: IDs survive a reorganisation of this document and
  section numbers do not. Review checks requirement citations (EN-02.1);
  `requirement-prefixes.txt` pins their prefix set (EN-02.4).
- The specification states what is true now — no dates, no version history, no
  measured values. It may state a normative gate ("within a CI-enforced
  budget") but not a measurement; a measured figure is a run record and lives
  beside the rig that produced it (`EN-03.2`).
- A behaviour change lands as one change: the specification row, a host test,
  and the affected documents together.
