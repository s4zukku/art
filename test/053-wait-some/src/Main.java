/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Exercise Object.wait(), comparing results against wall clock time.
 */
public class Main {
    /* delays, in milliseconds */
    private final static long[] DELAYS = {
        200, 500, 1000, 2000, 3500, 8000
    };

    public static void main(String[] args) {
        boolean timing = (args.length >= 1) && args[0].equals("--timing");
        doit(timing);
    }

    public static void doit(boolean timing) {
        Object sleepy = new Object();
        long start, end;

        synchronized (sleepy) {
            try {
                sleepy.wait(-500);
                System.out.println("HEY: didn't throw on negative arg");
            } catch (IllegalArgumentException iae) {
                System.out.println("Caught expected exception on neg arg");
            } catch (InterruptedException ie) {
                ie.printStackTrace(System.out);
            }

            for (long delay : DELAYS) {
                System.out.println("Waiting for " + delay + "ms...");

                start = System.currentTimeMillis();
                try {
                    sleepy.wait(delay);
                } catch (InterruptedException ie) {
                    ie.printStackTrace(System.out);
                }
                end = System.currentTimeMillis();

                long elapsed = end - start;
                boolean showTime = timing;

                if (! timing) {
                    // Allow a random scheduling delay of at least 100 msecs.
                    final long epsilon = Math.max(delay / 20, 100);
                    long min = delay - 1;
                    long max = delay + epsilon;

                    if (elapsed < min) {
                        // This can legitimately happen due to premature wake-ups.
                        // This seems rare and unexpected enough in practice that we should
                        // still report.
                        System.out.println("  Elapsed time was too short");
                        showTime = true;
                    } else if (elapsed > max) {
                        System.out.println("  Elapsed time was too long: "
                            + "elapsed=" + elapsed + " max=" + max);
                        showTime = true;
                    }
                }

                if (showTime) {
                    System.out.println("  Wall clock elapsed "
                            + elapsed + "ms");
                }
            }
        }
    }
}
