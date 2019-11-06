## Filmic Tonemapping

http://filmicworlds.com/blog/filmic-tonemapping-with-piecewise-power-curves/


```
x = clamp(channel-0.004, 0, 1);
toneMappedChannel = (x*(6.2*x+0.5))/(x*(6.2*x+1.7)+0.06);
```

http://advances.realtimerendering.com/s2011/Penner%20-%20Pre-Integrated%20Skin%20Rendering%20%28Siggraph%202011%20Advances%20in%20Real-Time%20Rendering%20Course%29.pptx

https://www.youtube.com/watch?v=m9AT7H4GGrA

Copyright (c) 2017-2018 the Volcano Authors. All rights reserved.

Hidden Treasure scene used in this example
[Copyright Laurynas Jurgila](http://www.blendswap.com/user/PigArt).
