# BC-250 carrier board: regenerate the KiCad project, check it, export fab files.
#
#   make            generate + check + export
#   make check      geometry check only (KiCad 7's kicad-cli has no DRC)
#   make export     gerbers (+ zip for JLCPCB), drill, PDFs, position file, netlist into out/
#   BOARDS=3 make   size the LCSC parts list (out/lcsc_parts.csv) for 3 boards (default 10)
#
# Needs python3 and kicad-cli 7 (KICAD_SHARE points at KiCad's share dir,
# /usr/share/kicad by default, for the stock symbols and footprints).

PCB = bc250_carrier.kicad_pcb
SCH = bc250_carrier.kicad_sch
OUT = out
CLI ?= kicad-cli
BOARDS ?= 10         # boards to order: sizes out/lcsc_parts.csv
export BOARDS

.PHONY: all generate check export clean

all: generate check export

generate:
	python3 generate.py

check:
	python3 check.py

export:
	rm -rf $(OUT)/gerbers && mkdir -p $(OUT)/gerbers
	$(CLI) pcb export gerbers --layers "F.Cu,B.Cu,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts" --output $(OUT)/gerbers/ $(PCB)
	$(CLI) pcb export drill --format excellon --excellon-separate-th --output $(OUT)/gerbers/ $(PCB)
	$(CLI) pcb export pdf --layers "F.Cu,B.Cu,F.SilkS,Edge.Cuts" --output $(OUT)/board-top.pdf $(PCB)
	$(CLI) pcb export pdf --layers "B.Cu,B.SilkS,Edge.Cuts" --mirror --output $(OUT)/board-bottom.pdf $(PCB)
	$(CLI) pcb export pos --format csv --units mm --output $(OUT)/positions.csv $(PCB)
	$(CLI) sch export pdf --output $(OUT)/schematic.pdf $(SCH)
	$(CLI) sch export netlist --output $(OUT)/netlist.net $(SCH)
	python3 -c "import zoom; zoom.crop('$(OUT)/board-top.pdf', '$(OUT)/zoom-top.pdf', False); zoom.crop('$(OUT)/board-bottom.pdf', '$(OUT)/zoom-bottom.pdf', True)"
	python3 -c "import shutil; shutil.make_archive('$(OUT)/bc250_carrier-gerbers', 'zip', '$(OUT)/gerbers')"

clean:
	rm -rf $(OUT)
