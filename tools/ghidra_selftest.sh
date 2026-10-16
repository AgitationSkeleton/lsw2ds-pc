#!/bin/sh
# Does Ghidra scripting actually work right now?
#
# WHY THIS IS A STANDING CHECK AND NOT A ONE-OFF. Ghidra script compilation
# silently stopped working at some point in this project's life, and nothing
# noticed for a long time. The cost was large and invisible: the reset vector
# was hand-translated, dropped call arguments were dug out of the image by hand,
# and 715 functions went missing from a re-export - all because the obvious fix,
# "ask Ghidra to decompile it properly", appeared to be unavailable.
#
# The failure mode is quiet. Scripts do not error in a way that says "the
# toolchain is broken"; they say "The class could not be found", which reads
# like a problem with the script. So this asks the question directly, and any
# session can run it in a few seconds.
#
#   sh tools/ghidra_selftest.sh
set -e
cd "$(dirname "$0")/.."

PROBE=tools/ghidra_scripts/GhidraSelfTest.java
cat > "$PROBE" <<'EOF'
import ghidra.app.script.GhidraScript;

public class GhidraSelfTest extends GhidraScript {
    @Override
    public void run() throws Exception {
        println("GHIDRA-SELFTEST-OK " + currentProgram.getName() + " functions="
                + currentProgram.getFunctionManager().getFunctionCount());
    }
}
EOF

out=$(powershell -ExecutionPolicy Bypass -File tools/ghidra_run.ps1 \
        -Script GhidraSelfTest.java \
        -Program arm9_section_1_01ff8000.bin 2>&1 || true)
rm -f "$PROBE"

if printf '%s' "$out" | grep -q 'GHIDRA-SELFTEST-OK'; then
    printf '%s\n' "$out" | grep -o 'GHIDRA-SELFTEST-OK.*'
    echo "ghidra scripting: WORKING"
    exit 0
fi

echo "ghidra scripting: BROKEN"
printf '%s\n' "$out" | grep -iE 'ERROR|could not be found|Exception' | head -3
cat <<'EOF'

What this has meant before, and how it was fixed:

  "Failed to get OSGi bundle containing script"
      External script directories do not register as bundles in this install.
      tools/ghidra_run.ps1 works around it by staging the script into Ghidra's
      own ghidra_scripts directory for the run. If that stopped working, check
      the staging path in that script first.

  A compile error naming a Ghidra class or constructor
      The API moved. Compile the script by hand to see the real diagnostic,
      which headless does not print:

        CP=$(find tools/vendor/ghidra_1212/Ghidra -name '*.jar' | tr '\n' ';')
        javac -cp "$CP" -d /tmp tools/ghidra_scripts/<Script>.java

      That is how FillGaps.java was found to be using a File(ResourceFile,
      String) constructor that no longer exists.
EOF
exit 1
