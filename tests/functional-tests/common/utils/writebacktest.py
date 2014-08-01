#!/usr/bin/python

# Copyright (C) 2010, Nokia (ivan.frade@nokia.com)
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.
#

from gi.repository import GLib

from common.utils.system import TrackerSystemAbstraction
import shutil
import unittest2 as ut
import os
from common.utils import configuration as cfg
from common.utils.helpers import log
import time

TEST_FILE_JPEG = "writeback-test-1.jpeg"
TEST_FILE_TIFF = "writeback-test-2.tif"
TEST_FILE_PNG = "writeback-test-4.png"

WRITEBACK_TMP_DIR = os.path.join (cfg.TEST_MONITORED_TMP_DIR, "writeback")

index_dirs = [WRITEBACK_TMP_DIR]
CONF_OPTIONS = {
    cfg.DCONF_MINER_SCHEMA: {
        'index-recursive-directories': GLib.Variant.new_strv(index_dirs),
        'index-single-directories': GLib.Variant.new_strv([]),
        'index-optical-discs': GLib.Variant.new_boolean(False),
        'index-removable-devices': GLib.Variant.new_boolean(False),
    }
}


def uri (filename):
    return "file://" + os.path.join (WRITEBACK_TMP_DIR, filename)

class CommonTrackerWritebackTest (ut.TestCase):
    """
    Superclass to share methods. Shouldn't be run by itself.
    Start all processes including writeback, miner pointing to HOME/test-writeback-monitored
    """
	     
    @classmethod
    def __prepare_directories (self):
        #
        #     ~/test-writeback-monitored/
        #
        
        for d in ["test-writeback-monitored"]:
            directory = os.path.join (WRITEBACK_TMP_DIR, d)
            if (os.path.exists (directory)):
                shutil.rmtree (directory)
            os.makedirs (directory)


        if (os.path.exists (os.getcwd() + "/test-writeback-data")):
            # Use local directory if available
            datadir = os.getcwd() + "/test-writeback-data"
        else:
            datadir = os.path.join (cfg.DATADIR, "tracker-tests",
                                    "test-writeback-data")

        for testfile in [TEST_FILE_JPEG, TEST_FILE_PNG,TEST_FILE_TIFF]:
            origin = os.path.join (datadir, testfile)
            log ("Copying %s -> %s" % (origin, WRITEBACK_TMP_DIR))
            shutil.copy (origin, WRITEBACK_TMP_DIR)
            time.sleep (2)

    
    @classmethod 
    def setUpClass (self):
        #print "Starting the daemon in test mode"
        self.__prepare_directories ()
        
        self.system = TrackerSystemAbstraction ()

        self.system.tracker_writeback_testing_start (CONF_OPTIONS)
        # Returns when ready
        log ("Ready to go!")
        
    @classmethod
    def tearDownClass (self):
        #print "Stopping the daemon in test mode (Doing nothing now)"
        self.system.tracker_writeback_testing_stop ()
    

    def get_test_filename_jpeg (self):
        return uri (TEST_FILE_JPEG)

    def get_test_filename_tiff (self):
        return uri (TEST_FILE_TIFF)

    def get_test_filename_png (self):
        return uri (TEST_FILE_PNG)
