# Manual Test Steps

Run all commands from the SemWeaver repository root.

```bash
export PROJECT_ROOT="$(pwd)"
export LAB_DIR="$PROJECT_ROOT/tests/tiny_buffer_lab"
export PATCH_PATH="$LAB_DIR/patches/tiny_copy_bounds_fix.patch"
export CONFIG_PATH="$PROJECT_ROOT/config/config.yaml"
export RUN_DIR="$PROJECT_ROOT/output/tiny_buffer_lab"

mkdir -p "$RUN_DIR"
```

## 1. Generate

```bash
python3 -m src.main \
  --config "$CONFIG_PATH" \
  generate \
  --patch "$PATCH_PATH" \
  --output "$RUN_DIR" \
  --validate-path "$LAB_DIR" \
  --analyzer csa \
  --verbose
```

## 2. Evidence

```bash
python3 -m src.main \
  --config "$CONFIG_PATH" \
  evidence \
  --patch "$PATCH_PATH" \
  --evidence-dir "$LAB_DIR" \
  --output "$RUN_DIR" \
  --analyzer csa \
  --verbose
```

## 3. Refine

```bash
python3 -m src.main \
  --config "$CONFIG_PATH" \
  refine \
  --input "$RUN_DIR" \
  --validate-path "$LAB_DIR" \
  --evidence-input "$RUN_DIR" \
  --patch "$PATCH_PATH" \
  --analyzer csa \
  --verbose
```

## 4. Manual CSA Confirmation

```bash
CHECKER_CPP="$(find "$RUN_DIR/refinements" "$RUN_DIR/csa" -name '*Checker.cpp' -print 2>/dev/null | sort | tail -n1)"
CHECKER_NAME="$(basename "$CHECKER_CPP" .cpp)"
CHECKER_SO="/tmp/tiny_buffer_lab_checker.so"

echo "checker cpp: $CHECKER_CPP"
echo "checker name: custom.$CHECKER_NAME"

/usr/lib/llvm-18/bin/clang++ \
  -shared -fPIC -std=c++20 -O2 \
  -I/usr/lib/llvm-18/include \
  -I/usr/lib/llvm-18/include/clang \
  -I/usr/lib/llvm-18/include/clang/StaticAnalyzer \
  -I/usr/lib/llvm-18/include/clang/StaticAnalyzer/Core \
  -I/usr/lib/llvm-18/include/clang/StaticAnalyzer/Frontend \
  -I/usr/lib/llvm-18/include/llvm \
  "$CHECKER_CPP" \
  -L/usr/lib/llvm-18/lib \
  -lclang-cpp \
  -Wl,-rpath,/usr/lib/llvm-18/lib \
  -o "$CHECKER_SO"

/usr/lib/llvm-18/bin/clang --analyze \
  -Xclang -load -Xclang "$CHECKER_SO" \
  -Xclang -analyzer-checker -Xclang "custom.$CHECKER_NAME" \
  -Xclang -analyzer-display-progress \
  -Xclang -analyzer-output=text \
  "$LAB_DIR/src/tiny_copy.c"
```

Expected result:
- A warning near `src/tiny_copy.c:13`.
- The message should describe an unchecked copy length or fixed-buffer bound risk.
