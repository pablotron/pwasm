;;
;; 08-call_indirect.wat: this module implements two functions which
;; test the call_indirect instruction:
;;
;; * ci.i32_map($val i32, $func_id i32) -> i32
;; * ci.f32_map($val f32, $func_id i32) -> f32
;;
(module
  ;; double i32 value
  (func $i32_x2 (param $val i32) ;; value
                (result i32)
    (i32.mul (i32.const 2) (local.get $val))
  )

  ;; triple i32 value
  (func $i32_x3 (param $val i32) ;; value
                (result i32)
    (i32.mul (i32.const 3) (local.get $val))
  )

  ;; square i32 value
  (func $i32_p2 (param $val i32) ;; value
                (result i32)
    (i32.mul (local.get $val) (local.get $val))
  )

  ;; cube i32 value
  (func $i32_p3 (param $val i32) ;; value
                (result i32)
    (i32.mul (local.get $val) (i32.mul (local.get $val) (local.get $val)))
  )

  ;; double f32 value
  (func $f32_x2 (param $val f32) ;; value
                (result f32)
    (f32.mul (f32.const 2) (local.get $val))
  )

  ;; triple f32 value
  (func $f32_x3 (param $val f32) ;; value
                (result f32)
    (f32.mul (f32.const 3) (local.get $val))
  )

  ;; square f32 value
  (func $f32_p2 (param $val f32) ;; value
                (result f32)
    (f32.mul (local.get $val) (local.get $val))
  )

  ;; cube f32 value
  (func $f32_p3 (param $val f32) ;; value
                (result f32)
    (f32.mul (local.get $val) (f32.mul (local.get $val) (local.get $val)))
  )

  ;; function table
  (table $fns funcref (elem
    $i32_x2 $i32_x3 $i32_p2 $i32_p3
    $f32_x2 $f32_x3 $f32_p2 $f32_p3
  ))

  (func $i32_map (param $val i32)     ;; value
                 (param $func_id i32) ;; function to apply
                 (result i32)
    (local.get $val)
    (local.get $func_id)
    (call_indirect (param i32) (result i32))
  )

  (export "i32_map" (func $i32_map))

  (func $f32_map (param $val f32)     ;; value
                 (param $func_id i32) ;; function to apply
                 (result f32)
    (local.get $val)
    (local.get $func_id)
    (call_indirect (param f32) (result f32))
  )

  (export "f32_map" (func $f32_map))
)
