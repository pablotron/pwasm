;;
;; 01-fib.wat: Module containing two functions which calculate the Nth
;; value of the Fibonacci sequence:
;;
;; * fib_recurse(i32) -> i32: Calculate the Nth value of the Fibonacci
;;   sequence, recursively.
;;
;; * fib_iterate(i32) -> i32: Calculate the Nth value of the Fibonacci
;;   sequence, iteratively.
;;
(module
  ;; fib_recurse: get Nth value of fibonacci sequence, recursively.
  (func $fib_recurse (param $num i32) (result i32)

    ;; n < 2
    (i32.lt_u (local.get $num) (i32.const 2))
    (if (result i32)
      (then
        ;; n < 2, return n
        local.get $num
      )

      (else
        ;; n >= 2, recurse

        ;; call fib(n - 2)
        (i32.sub (local.get $num) (i32.const 2))
        call $fib_recurse

        ;; call fib(n - 1)
        (i32.sub (local.get $num) (i32.const 1))
        call $fib_recurse

        ;; fib(n - 2) + fib(n - 1)
        i32.add
      )
    )
  )

  (export "fib_recurse" (func $fib_recurse))

  ;; fib_iterate: get Nth value of fibonacci sequence, iteratively.
  (func $fib_iterate (param $num i32) (result i32)
    (local $sum i32)  ;; cumulative sum
    (local $tmp i32)  ;; temp value

    ;; n < 2
    (i32.lt_u (local.get $num) (i32.const 2))
    (if (result i32)
      (then
        ;; n < 2, return n
        local.get $num
      )

      (else
        ;; n >= 2, iterate

        ;; decriment num
        (local.set $num (i32.sub (local.get $num) (i32.const 1)))

        ;; init sum and tmp
        (local.set $tmp (i32.const 0))
        (local.set $sum (i32.const 1))

        (loop (result i32)
          ;; cache last sum
          (local.get $sum)

          ;; increment/store sump
          (local.set $sum (i32.add (local.get $sum) (local.get $tmp)))

          ;; save last sum to tmp
          (local.set $tmp)

          ;; decriment num
          (local.tee $num (i32.sub (local.get $num) (i32.const 1)))

          ;; loop if num > 0
          (br_if 0)

          ;; return sum
          (local.get $sum)
        )
      )
    )
  )

  (export "fib_iterate" (func $fib_iterate))
)
