# Source through tools/drive.sh, 1360x860, with a separate virtual route fixture.
set -e
: "${NXTAKT_TEST_PORTS:?file containing four virtual JACK port names}"
: "${XDG_CONFIG_HOME:?isolated config directory}"
clk 1225 62
for i in 0 1 2 3; do
    port=$(sed -n "$((i+1))p" "$NXTAKT_TEST_PORTS")
    clk 650 $((261+i*66))
    clk 650 216
    xd type --clearmodifiers "$port"
    shot "port-$i"
    clk 650 267
done
shot selected-routes
clk 320 570
python3 - "$XDG_CONFIG_HOME/nxtakt/audio-settings.txt" "$NXTAKT_TEST_PORTS" "$LOG" "$REPO/build/audio_routing_test" <<"CHECK"
import re,sys,subprocess
config=open(sys.argv[1]).read()
expected=open(sys.argv[2]).read().splitlines()
assert [re.search(r"^jack"+str(i)+r" \"([^\"]*)\"",config,re.M).group(1) for i in range(4)]==expected,config
pid=re.search(r"engine: nxtaktd .* pid (\d+)",open(sys.argv[3]).read()).group(1)
subprocess.run([sys.argv[4],"--verify",pid,sys.argv[2]],check=True)
print("PASS: picker, apply, persistence and live routing")
CHECK
shot applied-routes
# Refresh must retain choices and live routes.
clk 460 570
shot refreshed-routes
# The audio view owns keyboard shortcuts while editing settings.
key space
shot no-transport-shortcut
# ALSA preferences save without touching the active JACK routes.
clk 880 186
clk 650 261
clk 650 216
xd type --clearmodifiers default
clk 650 267
clk 320 438
shot alsa-saved
# Close and reopen using the footer link.
clk 305 132
clk 1160 850
shot reopened-settings
