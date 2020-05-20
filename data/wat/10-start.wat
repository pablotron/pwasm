;;
;; 10-start.wat: test start function
;; 
;;
(module
  ;; test global value
  (global $val (mut i32) (i32.const 0))

  ;;
  ;; test func that sets $val to 42
  ;;
  (func $init
    (global.set $val (i32.add (global.get $val) (i32.const 42)))
  )

  ;; mark $init as start function
  (start $init)

  ;;
  ;; get global value
  ;;
  (func $get (result i32)
    (global.get $val)
  )

  ;; export $get
  (export "get" (func $get))
)
