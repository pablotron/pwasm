;;
;; 08-call_indirect.wat: this module implements a single function which
;; tests the call_indirect instruction:
;;
;; * ci.map($val i32, $func_id i32) -> i32
;;
;;
;;
(module
  ;; double value
  (func $x2 (param $val i32) ;; value
                (result i32)
    (i32.mul (i32.const 2) (local.get $val))
  )

  ;; triple value
  (func $x3 (param $val i32) ;; value
            (result i32)
    (i32.mul (i32.const 3) (local.get $val))
  )

  ;; square value
  (func $p2 (param $val i32) ;; value
            (result i32)
    (i32.mul (local.get $val) (local.get $val))
  )

  ;; cube value
  (func $p3 (param $val i32) ;; value
            (result i32)
    (i32.mul (local.get $val) (i32.mul (local.get $val) (local.get $val)))
  )

  ;; function table
  (table $fns funcref (elem $x2 $x3 $p2 $p3))

  (func $map (param $val i32)     ;; value
             (param $func_id i32) ;; function to apply
             (result i32)
    (call_indirect (local.get $id) (local.get $val))
  )

  (export "map" (func $map))
)
