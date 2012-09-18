mod_vhost_choke
===============

Apache vhost choke module to control the request/vhost mixture.


This apache module provides a multi-tenant (virtual hosted) apache
environment the ability to "choke" requests at a per-vhost level. 


Compilation
-----------
The "devel" is in the details -- so before you can compile this module,
ensure you have the apache headers (devel package) installed. 

    sudo {pkg-add -v apache-httpd-<ver> | yum install -y httpd-devel |
          apt-get install apache2-dev}

To compile and add module to apache:

    sudo apxs[2] -i -a -c mod_vhost_choke.c
    # Add options -Wc,-g -Wc,-O0  if you need 'em symbols.
    # Add options -Wc,-g -Wc,-O0 -Wc,-DVHC_DEV_DEBUG for dev debugging.


Configuration
-------------

These configuration directives are global and apply to the entire
environment (across virtual hosts).
Sample:  mod_vhost_choke.conf

    VHostChokeDebug  { On | Off } 
       -  Turns developer debug messages ON/OFF (default is Off)
          Debug messages are currently written to stderr.

    VHostChokeErrorCode  <http-code>
       -  Customized HTTP code to use when responding to choked requests.

    VHostChokeErrorMessage  "<message>"
       -  Customized error message (140 character limit) to use when
          responding to choked requests.



Default settings are: 

    VHostChokeDebug         Off
    VHostChokeErrorCode     429
    VHostChokeErrorMessage  "VirtualHost choked. Try again later."



Per-VirtualHost Configuration
-----------------------------

These configuration directives apply to individual vhosts.

    VHostChokeSlotLimit  <slot-limit>
       -  Request slot choke limit (range: 0-65535) for a Virtual Host
          (Default is 0 or unlimited slots)

    VHostChokeBurstPercent  <percentage>
       -  When VHostChokeSlotLimit is reached, the burst percentage
          controls how many additional slots are allowed for occasional
          request bursts. Setting this to 0 turns off bursting
          (Default is 30)

    VHostChokeGracePeriod  <num-seconds>
       -  When VHostChokeSlotLimit is reached, the grace period in seconds
          controls how long request bursts will get additional slots.
          Setting this to 0 turns off bursting (Default is 30 seconds)

    VHostChokeBurstFlapPeriod
       -  When VHostChokeSlotLimit is reached and bursting occured, this
          setting (in seconds - range: 0-65535) controls how long to wait
          before allowing bursts again. Prevents flapping - up/down within
          the burst period. Default is 1800 seconds or 30 minutes


Example: 

This configuration is a for virtual host called
pacman-ramr.example.org where we throttle the number of requests to 10 but
provide bursting capability of an additional 4 slots (4 grace) for a
maximum grace period of 90 seconds (1.5 minutes) and grace capability
is only granted once every hour (FlapPeriod is 60 minutes).

    <VirtualHost *:80>
       ServerName  pacman-ramr.example.org
       ServerAdmin bofh@example.org
       # ... 
       <IfModule mod_vhost_choke.c>
          #  10 concurrent requests.
          VHostChokeSlotLimit          10

          #  40% of 10 requests = 4 grace slots. Anything >= 31 would work
          #  as we do a ceil(percent/100 * slots).
          VHostChokeBurstPercent       40

          #  For 90 secs or 1.5 minutes.
          VHostChokeGracePeriod        90

          #  Once every hour.
          VHostChokeBurstFlapPeriod  3600
       </IfModule>
       #  ...
    </VirtualHost>


License
-------
The MIT License - see LICENSE file for more details.
