;;
;; 04-global.wat: Simple global test functions.
;;
(module
  (global $i32 (mut i32) (i32.const 0))
  (global $i64 (mut i64) (i64.const 0))
  (global $f32 (mut f32) (f32.const 0))
  (global $f64 (mut f64) (f64.const 0))

  (export "i32" (global $i32))
  (export "i64" (global $i64))
  (export "f32" (global $f32))
  (export "f64" (global $f64))

  ;;
  ;; get: get value of i32 global.
  ;;
  (func $i32_get (result i32) ;; return value
    (global.get $i32)
  )

  (export "i32.get" (func $i32_get))

  ;;
  ;; get: set value of i32 global.
  ;;
  (func $i32_set (param $val i32) ;; value
    (global.set $i32 (local.get $val))
  )

  (export "i32.set" (func $i32_set))

  ;;
  ;; get: get value of i64 global.
  ;;
  (func $i64_get (result i64) ;; return value
    (global.get $i64)
  )

  (export "i64.get" (func $i64_get))

  ;;
  ;; get: set value of i64 global.
  ;;
  (func $i64_set (param $val i64) ;; value
    (global.set $i64 (local.get $val))
  )

  (export "i64.set" (func $i64_set))

  ;;
  ;; get: get value of f32 global.
  ;;
  (func $f32_get (result f32) ;; return value
    (global.get $f32)
  )

  (export "f32.get" (func $f32_get))

  ;;
  ;; get: set value of f32 global.
  ;;
  (func $f32_set (param $val f32) ;; value
    (global.set $f32 (local.get $val))
  )

  (export "f32.set" (func $f32_set))

  ;;
  ;; get: get value of f64 global.
  ;;
  (func $f64_get (result f64) ;; return value
    (global.get $f64)
  )

  (export "f64.get" (func $f64_get))

  ;;
  ;; get: set value of f64 global.
  ;;
  (func $f64_set (param $val f64) ;; value
    (global.set $f64 (local.get $val))
  )

  (export "f64.set" (func $f64_set))
)
