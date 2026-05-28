Take a screenshot of a J2ME game running in the headless emulator and display it.

Usage: /screenshot [path/to/game.jar]
Default JAR: tests/g2048.jar

Steps:
1. Determine the JAR to use: if arguments are provided use "$ARGUMENTS", otherwise use "tests/g2048.jar"
2. Run these commands via WSL (the project root in WSL is /mnt/c/Users/Dziyana_Zavadskaya/Project/ALL/LEARN/AI/migrate/noJMe):

```bash
wsl -- bash -c "cd /mnt/c/Users/Dziyana_Zavadskaya/Project/ALL/LEARN/AI/migrate/noJMe && mkdir -p tests/baselines && make headless 2>&1 | tail -3 && timeout 5 ./bin/j2me-headless <JAR> 2>/dev/null; convert /tmp/bounce_framebuffer.ppm tests/baselines/<JAR_BASENAME>.png && echo OK"
```

Where `<JAR_BASENAME>` is the filename of the JAR without path and extension (e.g. `tests/g2048.jar` → `g2048`).

3. Read and display the resulting image at: C:\Users\Dziyana_Zavadskaya\Project\ALL\LEARN\AI\migrate\noJMe\tests\baselines\<jar-name>.png
