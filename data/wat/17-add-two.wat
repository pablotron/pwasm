;;
;; 16-add-two.wat: add two numbers together
;;
(module
  ;;
  ;; add_two: add 123 to 456 and return the result
  ;;   expect i32 579
  ;;
  (func $add_two (result i32)
    (i32.add (i32.const 123) (i32.const 456))
  )

  (export "add_two" (func $add_two))
)
