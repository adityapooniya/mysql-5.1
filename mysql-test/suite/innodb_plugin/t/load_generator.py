import cStringIO
import hashlib
import MySQLdb
import os
import random
import signal
import sys
import threading
import time
import string

CHARS = string.letters + string.digits

def sha1(x):
  return hashlib.sha1(str(x)).hexdigest()

def get_msg(do_blob):
  if do_blob:  
    blob_length = random.randint(1, 24000)
  else:
    blob_length = random.randint(1, 255)

  if random.randint(1, 2) == 1:
    # blob that cannot be compressed (well, compresses to 85% of original size)
    return ''.join([random.choice(CHARS) for x in xrange(blob_length)])
  else:
    # blob that can be compressed
    return random.choice(CHARS) * blob_length

def populate_table(con, num_records_before, do_blob, log):
  cur = con.cursor()
  stmt = None

  try:
    for i in xrange(num_records_before):
      msg = get_msg(do_blob)
      # print >> log, "length is %d, complen is %d" % (len(msg), len(zlib.compress(msg, 6)))
      stmt = """
INSERT INTO t1(id,msg_prefix,msg,msg_length,msg_checksum) VALUES (NULL,'%s','%s',%d,'%s')
""" % (msg[0:255], msg, len(msg), sha1(msg))
      cur.execute(stmt)

    con.commit()
    return True

  except MySQLdb.Error, e:
    print >> log, "cannot insert (%s) for statement (%s)" % (e, stmt)
    return False

def get_update(msg, idx):
  return """
UPDATE t1 SET msg_prefix='%s',msg='%s',msg_length=%d,msg_checksum='%s' WHERE id=%d""" % (
msg[0:255], msg, len(msg), sha1(msg), idx)

def get_insert_on_dup(msg, idx):
  return """
INSERT INTO t1 (msg_prefix,msg,msg_length,msg_checksum,id) VALUES ('%s','%s',%d,%s,%d)
ON DUPLICATE KEY UPDATE
msg_prefix=VALUES(msg_prefix),
msg=VALUES(msg),
msg_length=VALUES(msg_length),
msg_checksum=VALUES(msg_checksum),
id=VALUES(id)""" % (
msg[0:255], msg, len(msg), sha1(msg), idx)

def get_insert(msg, idx):
  return """
INSERT INTO t1 (msg_prefix,msg,msg_length,msg_checksum,id) VALUES ('%s','%s',%d,'%s',%d)""" % (
msg[0:255], msg, len(msg), sha1(msg), idx)

def get_insert_null(msg):
  return """
INSERT INTO t1 (msg_prefix,msg,msg_length,msg_checksum,id) VALUES ('%s','%s',%d,'%s',NULL)""" % (
msg[0:255], msg, len(msg), sha1(msg))

class Worker(threading.Thread):
  global LG_TMP_DIR

  def __init__(self, num_xactions, xid, con, server_pid, do_blob, max_id):
    threading.Thread.__init__(self)
    self.do_blob = do_blob
    self.xid = xid
    con.autocommit(False)
    self.con = con
    self.num_xactions = num_xactions
    cur = self.con.cursor()
    self.rand = random.Random()
    self.rand.seed(xid * server_pid)
    self.loop_num = 0
    self.max_id = max_id
    self.num_inserts = 0
    self.num_deletes = 0
    self.num_updates = 0
    self.time_spent = 0
    self.log = open('/%s/worker%02d.log' % (LG_TMP_DIR, self.xid), 'a')
#    print "num_inserts=%d,num_updates=%d,num_deletes=%d,time_spent=%d" %\
#(self.num_inserts, self.num_updates, self.num_deletes, self.time_spent)
    self.start()

  def finish(self):
    print >> self.log, "loop_num:%d, total time: %.2f s" % (
        self.loop_num, time.time() - self.start_time + self.time_spent)
    self.log.close()

  def validate_msg(self, msg_prefix, msg, msg_length, msg_checksum, idx):

    prefix_match = msg_prefix == msg[0:255]

    checksum = sha1(msg)
    checksum_match = checksum == msg_checksum

    len_match = len(msg) == msg_length

    if not prefix_match or not checksum_match or not len_match:
      errmsg = "id(%d), length(%s,%d,%d), checksum(%s,%s,%s) prefix(%s,%s,%s)" % (
          idx,
          len_match, len(msg), msg_length,
          checksum_match, checksum, msg_checksum,
          prefix_match, msg_prefix, msg[0:255])
      print >> self.log, errmsg

      cursor = self.con.cursor()
      cursor.execute("INSERT INTO errors VALUES('%s')" % errmsg)
      cursor.execute("COMMIT")
      raise Exception('validate_msg failed')
    else:
      print >> self.log, "Validated for length(%d) and id(%d)" % (msg_length, idx)

  def run(self):
    try:
      self.runme()
      print >> self.log, "ok, with do_blob %s" % self.do_blob
    except Exception, e:

      try:
        cursor = self.con.cursor()
        cursor.execute("INSERT INTO errors VALUES('%s')" % e)
        cursor.execute("COMMIT")
      except MySQLdb.Error, e2:
        print >> self.log, "caught while inserting error (%s)" % e2

      print >> self.log, "caught (%s)" % e
    finally:
      self.finish()

  def runme(self):
    self.start_time = time.time()
    cur = self.con.cursor()
    print >> self.log, "thread %d started, run from %d to %d" % (
        self.xid, self.loop_num, self.num_xactions)

    while not self.num_xactions or (self.loop_num < self.num_xactions):
      idx = self.rand.randint(0, self.max_id)
      insert_or_update = self.rand.randint(0, 3)
      self.loop_num += 1
      if self.rand.randint(0, 36) == 0:
        cur.execute("SET GLOBAL innodb_zlib_wrap=1-@@innodb_zlib_wrap");
      try:
        stmt = None

        msg = get_msg(self.do_blob)

        cur.execute("SELECT msg_prefix,msg,msg_length,msg_checksum FROM t1 WHERE id=%d" % idx)
        res = cur.fetchone()
        if res:
          self.validate_msg(res[0], res[1], res[2], res[3], idx)

        if insert_or_update:
          if res:
            if self.rand.randint(0, 1):
              stmt = get_update(msg, idx)
            else:
              stmt = get_insert_on_dup(msg, idx)
            self.num_updates += 1
          else:
            r = self.rand.randint(0, 2)
            if r == 0:
              stmt = get_insert(msg, idx)
            elif r == 1:
              stmt = get_insert_on_dup(msg, idx)
            else:
              stmt = get_insert_null(msg)
            self.num_inserts += 1
        else:
          stmt = "DELETE FROM t1 WHERE id=%d" % idx
          self.num_deletes += 1

        query_result = cur.execute(stmt)
        if (self.loop_num % 100) == 0:
          print >> self.log, "Thread %d loop_num %d: result %d: %s" % (self.xid,
                                                            self.loop_num, query_result,
                                                            stmt)

        # 30% commit, 10% rollback, 60% don't end the trx
        r = self.rand.randint(1,10)
        if r < 4:
          self.con.commit()
        elif r == 4:
          self.con.rollback()

      except MySQLdb.Error, e:
        if e.args[0] == 2006:  # server is killed
          print >> self.log, "mysqld down, transaction %d" % self.xid
          return
        else:
          print >> self.log, "mysql error for stmt(%s) %s" % (stmt, e)

    try:
      self.con.commit()
    except Exception, e:
      print >> self.log, "commit error %s" % e

if  __name__ == '__main__':
  global LG_TMP_DIR

  pid_file = sys.argv[1]
  kill_db_after = int(sys.argv[2])
  num_records_before = int(sys.argv[3])
  num_workers = int(sys.argv[4])
  num_xactions_per_worker = int(sys.argv[5])
  user = sys.argv[6]
  host = sys.argv[7]
  port = int(sys.argv[8])
  db = sys.argv[9]
  do_blob = int(sys.argv[10])
  max_id = int(sys.argv[11])
  LG_TMP_DIR = sys.argv[12]
  workers = []
  server_pid = int(open(pid_file).read())
  log = open('/%s/main.log' % LG_TMP_DIR, 'a')

#  print  "kill_db_after = ",kill_db_after," num_records_before = ", \
#num_records_before, " num_workers= ",num_workers, "num_xactions_per_worker =",\
#num_xactions_per_worker, "user = ",user, "host =", host,"port = ",port,\
#" db = ", db, " server_pid = ", server_pid

  if num_records_before:
    print >> log, "populate table do_blob is %d" % do_blob
    con = MySQLdb.connect(user=user, host=host, port=port, db=db)
    if not populate_table(con, num_records_before, do_blob, log):
      sys.exit(1)
    con.close()

  print >> log, "start %d threads" % num_workers
  for i in xrange(num_workers):
    worker = Worker(num_xactions_per_worker, i,
                    MySQLdb.connect(user=user, host=host, port=port, db=db),
                    server_pid, do_blob, max_id)
    workers.append(worker)

  if kill_db_after:
    print >> log, "kill mysqld"
    time.sleep(kill_db_after)
    os.kill(server_pid, signal.SIGKILL)

  print >> log, "wait for threads"
  for w in workers:
    w.join()

  print >> log, "all threads done"

