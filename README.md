# OSC Final: Simple SSD

## Run

```
$ make
$ mkdir /tmp/ssd 
$ make run
...
# open another shell
$ sh test.sh test1
```

## Implementation
* Read
    * ssd_do_read -> **ftl_read** -> nand_read
* Write
    * ssd_do_write -> **ftl_write** -> nand_write
* GC
    * Select block with least valid page
    * The selected block should not be updated recently