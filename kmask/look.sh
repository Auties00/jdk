#!/bin/bash
# Runs in the workspace root on a runner with AVX-512: base-{release,debug} are the bundles of the notrap branch
# before, new-{release,debug} those of the cost-kmask branch.
home() { (cd "$(dirname "$(find "$1" -name java -path '*/bin/*' -type f | head -1)")/.." && pwd); }
BR=$(home base-release); NR=$(home new-release); ND=$(home new-debug); BD=$(home base-debug)
grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //' | tee out/cpu.txt
"$NR/bin/java" -XX:+UnlockDiagnosticVMOptions -XX:+PrintFlagsFinal -version 2>&1 | grep -E ' (UseAVX|MaxVectorSize|AvoidLosableStateAtTraps) |version' | tee out/flags.txt
m2=https://repo1.maven.org/maven2
for a in org/openjdk/jmh/jmh-core/1.37/jmh-core-1.37.jar org/openjdk/jmh/jmh-generator-annprocess/1.37/jmh-generator-annprocess-1.37.jar net/sf/jopt-simple/jopt-simple/5.0.4/jopt-simple-5.0.4.jar org/apache/commons/commons-math3/3.6.1/commons-math3-3.6.1.jar; do
  curl -sfL -o "lib-$(basename $a)" "$m2/$a" || echo "download failed: $a"
done
cp="lib-jmh-core-1.37.jar:lib-jopt-simple-5.0.4.jar:lib-commons-math3-3.6.1.jar"
"$BR/bin/javac" --add-modules jdk.incubator.vector -proc:full -cp "$cp:lib-jmh-generator-annprocess-1.37.jar" -d jmhc src/review9/*.java 2>&1 | grep -v -i warning | tail -5
COMMON="--add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow -XX:+UnlockDiagnosticVMOptions -XX:+AvoidLosableStateAtTraps"

# 1. The release JVMs, many forks, with the decisions of the mask pass logged in each fork (N), for the kernels
#    whose forks differ.
for c in N T F; do
  J="$NR"; [ $c != N ] && J="$BR"
  f="-XX:+AvoidLosableStateAtTraps"; [ $c = F ] && f="-XX:-AvoidLosableStateAtTraps"
  "$J/bin/java" -cp "jmhc:$cp" org.openjdk.jmh.Main -jvmArgsAppend "$COMMON $f -Xlog:jit+compilation=debug"     -rf csv -rff "out/forks-$c.csv" -f 8 -wi 3 -i 3 -w 1 -r 1 "tailMaskedCopy|compareAcrossLoadMaskedStore|unsafeWithLiveMask" > "out/forks-$c-full.log" 2>&1
  # keep the fork markers, the results and the lines of the pass and of the kernels' compilations
  grep -E "^# Fork|^# Benchmark:|^Iteration|^review9|no-trap|R10Bench::|R9Bench::|made not entrant" "out/forks-$c-full.log" > "out/forks-$c.log"
  rm -f "out/forks-$c-full.log"
  grep -E "^# Fork|^# Benchmark:|^Iteration   3|no-trap.*(tailMasked|compareAcross|unsafeWith|retrying)" "out/forks-$c.log" | sed 's/^.*no-trap/no-trap/' | cut -c1-160
done

# 2. PrintOptoAssembly with the fastdebug JVMs: before with the mode off (F) and on (T), cost-kmask (N)
for c in F T N; do
  f=-XX:-AvoidLosableStateAtTraps; [ $c != F ] && f=-XX:+AvoidLosableStateAtTraps
  J="$BD"; [ $c = N ] && J="$ND"
  "$J/bin/java" -cp "jmhc:$cp" org.openjdk.jmh.Main \
    -jvmArgsAppend "--add-modules jdk.incubator.vector --enable-native-access=ALL-UNNAMED --sun-misc-unsafe-memory-access=allow -XX:+UnlockDiagnosticVMOptions $f -Xlog:jit+compilation=debug -XX:CompileCommand=PrintOptoAssembly,review9.*::*" \
    -f 1 -wi 5 -i 1 -w 1 -r 1 "review9" > "out/opto-avx3-$c.log" 2>&1
  grep "no-trap:" "out/opto-avx3-$c.log" | sed 's/^.*no-trap/no-trap/' | sort | uniq -c | sort -rn | head -30
done
