#!/bin/bash
# Runs in the workspace root: base-{release,debug} are the bundles of the base, new-{release,debug} the same
# with the libjvm.so under test. SLOT is the matrix slot: opto, jtreg or jmh<n>.
BENCH_AVX="3"
JMH_ARGS="-f 3 -wi 3 -i 4 -w 1 -r 1"
JTREG_TESTS="test/hotspot/jtreg/compiler/runtime/safepoints test/hotspot/jtreg/compiler/vectorapi test/jdk/java/foreign/TestSegments.java test/jdk/java/foreign/TestByteBuffer.java"

home() { (cd "$(dirname "$(find "$1" -name java -path '*/bin/*' -type f | head -1)")/.." && pwd); }
BR=$(home base-release); NR=$(home new-release); ND=$(home new-debug); BD=$(home base-debug)
JT=$(dirname "$(find jtreg -name jtreg.jar | head -1)")
grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //' | tee out/cpu.txt
"$NR/bin/java" -XX:+UnlockDiagnosticVMOptions -XX:+PrintFlagsFinal -version 2>&1 | grep -E ' (UseAVX|MaxVectorSize|AvoidLosableStateAtTraps) |version' | tee out/flags.txt
"$ND/bin/java" -version 2>&1 | tee -a out/flags.txt
m2=https://repo1.maven.org/maven2
for a in org/openjdk/jmh/jmh-core/1.37/jmh-core-1.37.jar org/openjdk/jmh/jmh-generator-annprocess/1.37/jmh-generator-annprocess-1.37.jar net/sf/jopt-simple/jopt-simple/5.0.4/jopt-simple-5.0.4.jar org/apache/commons/commons-math3/3.6.1/commons-math3-3.6.1.jar; do
  curl -sfL -o "lib-$(basename $a)" "$m2/$a" || echo "download failed: $a"
done
cp="lib-jmh-core-1.37.jar:lib-jopt-simple-5.0.4.jar:lib-commons-math3-3.6.1.jar"
"$BR/bin/javac" --add-modules jdk.incubator.vector -proc:full -cp "$cp:lib-jmh-generator-annprocess-1.37.jar" -d jmhc src/review9/*.java 2>&1 | grep -v -i warning | tail -5
COMMON="--add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow -XX:+UnlockDiagnosticVMOptions"

case "$SLOT" in
opto)
  # PrintOptoAssembly: the base with the mode off (F) and on (T), the JVM under test with the mode on (N)
  for avx in 3 2; do
    for c in F T N; do
      f=-XX:-AvoidLosableStateAtTraps; [ $c != F ] && f=-XX:+AvoidLosableStateAtTraps
      J="$BD"; [ $c = N ] && J="$ND"
      "$J/bin/java" -cp "jmhc:$cp" org.openjdk.jmh.Main \
        -jvmArgsAppend "$COMMON $f -XX:UseAVX=$avx -Xlog:jit+compilation=debug -XX:CompileCommand=PrintOptoAssembly,review9.*::*" \
        -f 1 -wi 5 -i 1 -w 1 -r 1 "review9" > "out/opto-avx$avx-$c.log" 2>&1
      grep "no-trap:" "out/opto-avx$avx-$c.log" | sed 's/^.*no-trap/no-trap/' | sort | uniq -c | sort -rn | head -30
      grep -E "^review9.*avgt" "out/opto-avx$avx-$c.log"
    done
  done
  # A short JMH pass with the release JVMs, for a first look
  for c in F T N; do
    J="$BR"; [ $c = N ] && J="$NR"
    f=-XX:-AvoidLosableStateAtTraps; [ $c != F ] && f=-XX:+AvoidLosableStateAtTraps
    "$J/bin/java" -cp "jmhc:$cp" org.openjdk.jmh.Main -jvmArgsAppend "$COMMON $f" \
      -rf csv -rff "out/quick-$c.csv" -f 1 -wi 3 -i 4 -w 1 -r 1 "review9" > "out/quick-$c.log" 2>&1
    grep -E "^review9.*avgt" "out/quick-$c.log"
  done
  ;;
jtreg)
  # jtreg, mode forced on, fastdebug
  (cd src && "$ND/bin/java" -jar "$JT/jtreg.jar" -jdk:"$ND" -w ../wk -r ../rp -timeoutFactor:4 -verbose:summary -conc:4 \
    -vmoptions:"-XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps" $JTREG_TESTS) > out/jtreg.txt 2>&1
  grep -E '^(FAILED|Error|Test results)' out/jtreg.txt | tail -40
  mkdir -p out/jtr; grep -l -r -E "status=(Failed|Error)" wk --include='*.jtr' 2>/dev/null | head -20 | while read -r j; do cp "$j" out/jtr/; done
  find wk -name 'hs_err*.log' 2>/dev/null | head -5 | while read -r j; do cp "$j" out/jtr/; done
  # the test of the decisions of the mask pass needs a flagless run
  (cd src && "$ND/bin/java" -jar "$JT/jtreg.jar" -jdk:"$ND" -w ../wk2 -r ../rp2 -timeoutFactor:4 -verbose:summary     test/hotspot/jtreg/compiler/runtime/safepoints/TestMaskKillAtRawAccesses.java) > out/jtreg-kills.txt 2>&1
  grep -E '^(Passed|FAILED|Error|Test results)' out/jtreg-kills.txt | tail -8
  find wk2 -name '*.jtr' 2>/dev/null | while read -r j; do cp "$j" out/jtr/; done
  ;;
jmh*)
  # JMH: base mode off (F), base mode on (T), new mode on (N), on the same machine in interleaved rounds
  declare -A JAVA FLAGS
  JAVA[F]="$BR/bin/java"; FLAGS[F]="-XX:-AvoidLosableStateAtTraps"
  JAVA[T]="$BR/bin/java"; FLAGS[T]="-XX:+AvoidLosableStateAtTraps"
  JAVA[N]="$NR/bin/java"; FLAGS[N]="-XX:+AvoidLosableStateAtTraps"
  JAVA[M]="$(home prev-release)/bin/java"; FLAGS[M]="-XX:+AvoidLosableStateAtTraps"
  for avx in $BENCH_AVX; do
    for round in 1 2 3; do
      order="F T M N"; [ $round = 2 ] && order="N F T M"; [ $round = 3 ] && order="T M N F"
      for c in $order; do
        "${JAVA[$c]}" -cp "jmhc:$cp" org.openjdk.jmh.Main -jvmArgsAppend "$COMMON ${FLAGS[$c]} -XX:UseAVX=$avx" \
          -rf csv -rff "out/jmh-$c-avx$avx-$round.csv" $JMH_ARGS "review9" > "out/jmh-$c-avx$avx-$round.log" 2>&1
        grep -E "^review9.*avgt" "out/jmh-$c-avx$avx-$round.log"
      done
    done
  done
  ;;
esac
