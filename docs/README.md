# pico-bno085 documentation

| Document | Purpose | Read it when |
|---|---|---|
| [traceability-exceptions.txt](traceability-exceptions.txt) | Requirements no source file can cite | Reviewing uncitable requirements |
| [orientation.md](orientation.md) | Local build settings, layout and invariants | Starting work in this repository |
| [technical-specification.md](technical-specification.md) | Normative requirements, pass criteria and traceability matrix | Checking or changing a guarantee |
| [architecture.md](architecture.md) | Driver, HAL and transport design | Deciding where a change belongs |
| [usage-guide.md](usage-guide.md) | Wiring, bus selection and startup | Bringing up a board |
| [future-work.md](future-work.md) | Deferred work and evidence gaps | Checking why something is absent |
| [deviations.md](deviations.md) | Standing deviations and their review dates | Reviewing a suppression |
| [deviation-budget.txt](deviation-budget.txt) | Suppression ceilings by language | Reviewing suppression counts |
| [requirement-prefixes.txt](requirement-prefixes.txt) | Requirement prefix set | Adding a prefix |
| [external-citations.txt](external-citations.txt) | Cross-repository requirement citations and owners | Citing a dependency's requirement |
| [Doxyfile](Doxyfile) | API reference configuration | Building or checking the reference |
| [GitHub Releases](https://github.com/jamesfxley/pico-bno085/releases) | Release changes and migration instructions | Upgrading |

The specification is the only normative document here. A behaviour described
elsewhere is a commitment only if the specification requires it. Its matrix
names the evidence that establishes each requirement.

[Device measurements](../examples/bench/baselines/) live with the benchmark.

## The datasheet

Electrical and timing claims cite CEVA 1000-3927, *BNO08X Datasheet*, v1.17.
The manual is obtained from CEVA and is not redistributed. Citations name the
document and section. pico-sh2 cites its SH-2 and SHTP sources the same way.

## The API reference

Doxygen generates the reference from public headers:

```sh
doxygen docs/Doxyfile      # -> docs/api/html/index.html (gitignored)
```

The `api-docs` gate fails on undocumented public entities. CI runs it on pull
requests and pushes to `main`.

## Conventions

Document structure and citation conventions follow
[EN-03](../infra/house/standard/engineering/03-documentation.md) and
[EN-02](../infra/house/standard/engineering/02-spec-and-traceability.md).
The specification may state a normative gate but carries no measurements
(EN-03.2). Behaviour changes follow EN-02.6. Deferred work follows EN-03.7;
the issue tracker holds defects.
