;;
;; 03-mem.wat: Simple memory access test functions.
;;
(module
  (memory $mem (export "mem") 1)

  ;;
  ;; get: get i32 at given address.
  ;;
  (func $get (param $pos i32) ;; address
             (result i32) ;; return value
    (i32.load (local.get $pos))
  )

  (export "get" (func $get))

  ;;
  ;; set: set i32 value at given address and return value.
  ;;
  (func $set (param $pos i32) ;; address
             (param $val i32) ;; value
             (result i32) ;; return value
    (i32.store (local.get $pos) (local.get $val))
    (local.get $val)
  )
)
