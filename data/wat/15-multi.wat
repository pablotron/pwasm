;;
;; multi-param/result tests
;;
(module
  ;;
  ;; test_block_results
  ;;   expect i32 11
  ;;
  (func $test_block_results (result i32)
    (block (result i32) (result i32)
      (i32.const 2)
      (i32.const 9)
    )
    (i32.add)
  )

  (export "test_block_results" (func $test_block_results))

  ;;
  ;; test_block_params
  ;;   expect i32 21
  ;;
  (func $test_block_params (result i32)
    (i32.const 1)
    (i32.const 2)
    (block (param i32) (param i32) (result i32)
      (i32.add)
      (i32.const 7)
      (i32.mul)
    )
  )

  (export "test_block_params" (func $test_block_params))
)
