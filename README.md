# potato_regex
A C regex library that works on tiny computers

Definitely a work in progress.  

## Features

    # Character class
    [abc]

    # Character class with range
    [a-zA-z0-9]

    # Negated character class
    [^abc]

    * Group
    (a|b|c)

    Characters
    \d \D \w \W \s \S .

    Multipliers
    + * ?

## Read stuff

### Papers
- [OG Ken Thompson](https://dl.acm.org/doi/10.1145/363347.363387)
- [Russ Cox](https://swtch.com/~rsc/regexp/regexp1.html)


### Implementations
- [https://gist.github.com/gmenard/6161825](https://gist.github.com/gmenard/6161825)
- [https://gist.github.com/DmitrySoshnikov/1239804/ba3f22f72d7ea00c3a662b900ded98d344d46752](https://gist.github.com/DmitrySoshnikov/1239804/ba3f22f72d7ea00c3a662b900ded98d344d46752)
- [https://www.youtube.com/watch?v=QzVVjboyb0s](https://www.youtube.com/watch?v=QzVVjboyb0s)
