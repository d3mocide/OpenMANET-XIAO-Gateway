# components/

Empty on purpose. The Morse Micro HaLow component (`morsemicro/halow`) is
pulled from the ESP Component Registry via `main/idf_component.yml`, not
vendored here - `idf.py build` fetches it into `managed_components/` (not
checked into this repo).

Use this directory only if a component ever needs to be vendored directly
instead (e.g. a local fork/patch of `morsemicro/halow`).
