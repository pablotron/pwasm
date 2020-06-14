;;
;; 16-mem-init.wat: Test memory initialization
;;
(module
  (memory $mem (data "\03\01\04\01\05\09\02\06"))

  ;;
  ;; get: get byte at given byte offset.
  ;;
  (func $get (param $pos i32) ;; address
             (result i32) ;; return value
    (i32.load8_u (local.get $pos))
  )

  (export "get" (func $get))
)
