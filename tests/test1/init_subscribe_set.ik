subscribe set (id = 1, provider = 1, receiver = 2, forward = yes);
date;
echo 'sleep a couple of seconds...';
sleep (seconds = 2);
date (format='%Y-%m-%d %H:%M:%S %Z');
echo 'done sleeping...';
