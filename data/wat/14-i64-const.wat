;;
;; i64.const tests
;;
;; Note: If I'm reading the spec correctly, i32 and i64 literals in the
;; text format are signed, and unspecified (which defaults to unsigned)
;; in the binary format.
;;
;; If so, then these tests should all generate ;; negative values, even
;; though I want the values to be large literal values.
;;
(module
  ;;
  ;; test_high_byte
  ;;   expect i64 0xFF00000000000000
  ;;
  (func $test_high_byte (result i64)
    (i64.const 0xFF00000000000000)
  )

  (export "test_high_byte" (func $test_high_byte))

  ;;
  ;; test_cycle_bytes
  ;;   expect i64 0xFF00FF00FF00FF00
  ;;
  (func $test_cycle_bytes (result i64)
    (i64.const 0xFF00FF00FF00FF00)
  )

  (export "test_cycle_bytes" (func $test_cycle_bytes))

  ;;
  ;; test_all_bytes
  ;;   expect i64 0xFFFFFFFFFFFFFFFF
  ;;
  (func $test_all_bytes (result i64)
    (i64.const 0xFFFFFFFFFFFFFFFF)
  )

  (export "test_all_bytes" (func $test_all_bytes))
)
