;;
;; multi-param/result tests
;;
(module
  ;;
  ;; test_block_result
  ;;   expect i32 11
  ;;
  (func $test_block_result (result i32)
    (block (result i32) (result i32)
      (i32.const 2)
      (i32.const 9)
    )
    (i32.add)
  )

  (export "test_block_result" (func $test_block_result))
)
