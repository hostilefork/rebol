; The SYM-PATH! type is new and needs testing.

(sym-path! = type of '@a/b/c)
('a = first '@a/b/c)
(3 = length of '@a/b/c)
("@a/b/c" = (mold to sym-path! [a b c]))

(obj: make object! [x: 10], (just '10) = @obj/x)
(obj: make object! [x: null], null = @obj/x)
