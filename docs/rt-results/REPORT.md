PREUVE RT : 38000 cycles, 591 dépassements, gigue max 85710.1 us, latence token p99 92.03 ms

# Cadence -- preuve temps reel (QNX 8.0, invite KVM)

## 1. Verdict

| critere | valeur |
|---|---|
| cycles de controle (phase LLM isole) | 38000 |
| depassements de periode | 591 |
| gigue max | 85710.1 us |
| gigue p99 | 2268.0 us |
| latence token p99 | 92.03 ms |
| debit LLM | 13.71 t/s |
| tokens generes | 300 |

## 2. Tache de controle -- jitter et depassements

| phase | prio | cpu | periode(us) | cycles | depassements | jitter min(us) | jitter moy(us) | jitter p99(us) | jitter max(us) | travail max(us) |
|---|---|---|---|---|---|---|---|---|---|---|
| idle | 63 | 0 | 1000.0 | 15000 | 40 | 167.0 | 313.0 | 336.2 | 10161.9 | 34.2 |
| llm_isolated | 63 | 0 | 1000.0 | 38000 | 591 | 2.7 | 1055.2 | 2268.0 | 85710.1 | 48.5 |
| llm_shared | 10 | -1 | 1000.0 | 18000 | 6072 | 18.5 | 1126.5 | 3589.4 | 5020.9 | 15.8 |

## 3. LLM -- latence par token

| phase | threads | tokens | chargement(s) | generation(s) | debit(t/s) | lat min(ms) | lat moy(ms) | lat p99(ms) | lat max(ms) |
|---|---|---|---|---|---|---|---|---|---|
| llm | 5 | 300 | 1.16 | 21.88 | 13.71 | 60.39 | 69.94 | 92.03 | 111.04 |
| llm | 6 | 200 | 0.97 | 12.73 | 15.71 | 55.55 | 59.18 | 75.06 | 90.39 |

## 4. Trace kernel (tracelogger / traceprinter)

| evenement (classe:evenement) | nombre |
|---|---|
| THREAD:THRUNNING | 138988 |
| THREAD:THREADY | 124096 |
| THREAD:THINTR | 46205 |
| SYSTEM:SEND_IPI_64 | 40610 |
| SYSTEM:IPI_64 | 30929 |
| THREAD:THRECEIVE | 15422 |
| INTERRUPT:INT_DELIVER | 15269 |
| THREAD:THREPLY | 14889 |
| THREAD:THNANOSLEEP | 8772 |
| SYSTEM:TIMER | 8772 |
| SYSTEM:MMAP | 958 |
| CONTROL:TIME | 555 |
| CONTROL:BUFFER | 519 |
| THREAD:THTRACEBUFFER | 519 |
| CONTROL:BUFFER_END | 519 |

 interruptions: 15269 | preemptions: 46205 | duree trace: 12.000 s

## 5. Fichiers

- `REPORT.md`
- `report.json`
- `serial-rt.log`
- `trace-metrics.json`
- `trace.kev`
- `trace.txt`
- `docs/REAL-TIME-PROOF.md`
