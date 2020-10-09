(in-package #:clasp-cleavir-translate-bir)

;;;; Variables and accessors used in the translation process.
;;;; In a separate file because translate-bir and landing-pad-bir both need
;;;; these and circular dependencies are bad.

(defvar *tags*)
(defvar *datum-values*)
(defvar *variable-allocas*)
(defvar *dynenv-storage*)
(defvar *unwind-ids*)
(defvar *compiled-enters*)
(defvar *function-enclose-lists*)
(defvar *enclose-initializers*)

(defun delay-initializer (initializer-thunk)
  (push initializer-thunk *enclose-initializers*))

(defun force-initializers ()
  (loop (unless *enclose-initializers*
          (return))
        (funcall (pop *enclose-initializers*))))

(defun iblock-tag (iblock)
  (or (gethash iblock *tags*)
      (error "BUG: No tag for iblock: ~a" iblock)))

(defun in (datum)
  (check-type datum (or cleavir-bir:phi cleavir-bir:ssa))
  (or (gethash datum *datum-values*)
      (and (typep datum 'cleavir-bir:immediate)
           (cmp:irc-int-to-ptr
            (clasp-cleavir::%i64 (cleavir-bir:immediate-value datum))
            cmp:%t*%))
      (error "BUG: No variable for datum: ~a" datum)))

(defun variable-in (variable)
  (check-type variable cleavir-bir:variable)
  (if (cleavir-bir:closed-over-p variable)
      (if (cleavir-bir:immutablep variable)
          (or (gethash variable *datum-values*)
              (error "BUG: Closure variable missing: ~a" variable))
          (let ((cell (or (gethash variable *datum-values*)
                          (error "BUG: Cell missing: ~a" variable)))
                (offset (- cmp:+cons-car-offset+ cmp:+cons-tag+)))
            (cmp:irc-load-atomic (cmp::gen-memref-address cell offset))))
      (if (cleavir-bir:immutablep variable)
          (or (gethash variable *datum-values*)
              (error "BUG: Variable missing: ~a" variable))
          (let ((alloca (or (gethash variable *variable-allocas*)
                            (error "BUG: Variable missing: ~a" variable))))
            (cmp:irc-load alloca)))))

(defun out (value datum)
  (check-type datum cleavir-bir:transfer)
  (assert (not (gethash datum *datum-values*))
          ()
          "Double OUT for ~a: Old value ~a, new value ~a"
          datum (gethash datum *datum-values*) value)
  (setf (gethash datum *datum-values*) value))

(defun phi-out (value datum llvm-block)
  (check-type datum cleavir-bir:phi)
  (unless (eq (cleavir-bir:rtype datum) :multiple-values)
    (llvm-sys:add-incoming (in datum) value llvm-block)))

(defun variable-out (value variable)
  (check-type variable cleavir-bir:variable)
  (if (cleavir-bir:closed-over-p variable)
      (if (cleavir-bir:immutablep variable)
          (setf (gethash variable *datum-values*) value)
          (let ((cell (or (gethash variable *datum-values*)
                          (error "BUG: Cell missing: ~a" variable)))
                (offset (- cmp:+cons-car-offset+ cmp:+cons-tag+)))
            (cmp:irc-store-atomic
             value
             (cmp::gen-memref-address cell offset))))
      (if (cleavir-bir:immutablep variable)
          (setf (gethash variable *datum-values*) value)
          (let ((alloca (or (gethash variable *variable-allocas*)
                            (error "BUG: Variable missing: ~a" variable))))
            (cmp:irc-store value alloca)))))

(defun dynenv-storage (dynenv)
  (check-type dynenv cleavir-bir:dynamic-environment)
  (or (gethash dynenv *dynenv-storage*)
      (error "BUG: Missing dynenv storage for ~a" dynenv)))

(defun (setf dynenv-storage) (new dynenv)
  (setf (gethash dynenv *dynenv-storage*) new))

(defun get-destination-id (iblock)
  (or (gethash iblock *unwind-ids*)
      (error "Missing unwind ID for ~a" iblock)))

(defun function-enclose-list (code)
  (or (gethash code *function-enclose-lists*)
      (setf (gethash code *function-enclose-lists*)
            (cleavir-set:set-to-list (cleavir-bir:environment code)))))
