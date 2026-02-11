# Allows PlatformIO to upload directly to ElegantOTA
#
# To use:
# - copy this script into the same folder as your platformio.ini
# - set the following for your project in platformio.ini:
#
# extra_scripts = platformio_upload.py
# upload_protocol = custom
# upload_url = <your upload URL>
# 
# An example of an upload URL:
#                upload_url = http://192.168.1.123/update 
# also possible: upload_url = http://domainname/update

import requests
import hashlib
from urllib.parse import urlparse
import time
import os
Import("env")

try:
    from requests_toolbelt import MultipartEncoder, MultipartEncoderMonitor
    from tqdm import tqdm
except ImportError:
    env.Execute("$PYTHONEXE -m pip install requests_toolbelt")
    env.Execute("$PYTHONEXE -m pip install tqdm")
    from requests_toolbelt import MultipartEncoder, MultipartEncoderMonitor
    from tqdm import tqdm

def on_upload(source, target, env):
    file_path = str(source[0])
#    upload_url_compatibility = env.GetProjectOption('upload_url')
#    upload_url = upload_url_compatibility.replace("/update", "")

    upload_url = env.GetProjectOption('upload_url')

    print("Upload url: " + upload_url);
    print("Firmare path: " + file_path);
    
    file_size = os.path.getsize(file_path)
    print("File size: " + str(file_size) + " bytes")

    with open(file_path, 'rb') as firmware:
        parsed_url = urlparse(upload_url)
        host_ip = parsed_url.netloc

        # Führe die GET-Anfrage aus
#        start_url = f"{upload_url}/ota/start?mode=fr&hash={md5}"
        start_url = f"{upload_url}"
        start_headers = {
            'Host': host_ip,
            'User-Agent': 'Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:109.0) Gecko/20100101 Firefox/118.0',
            'Accept': '*/*',
            'Accept-Language': 'de,en-US;q=0.7,en;q=0.3',
            'Accept-Encoding': 'gzip, deflate',
            'Referer': f'{upload_url}/update',
            'Connection': 'keep-alive'
            }

        print("Connecting...");

        start_response = requests.get(start_url, headers=start_headers)
        
        if start_response.status_code != 200:
            print("\nUpload failed.")
            return 1

        file_data = firmware.read()

        # Define headers
        headers = {
            'X-Requested-With': 'XMLHttpRequest',
            'Content-Type': 'application/octet-stream',  # Set the appropriate content type
        }

        print("Sending firmware...");

        # Send the POST request with the MultipartEncoderMonitor
        response = requests.post(upload_url, data=file_data, headers=headers)

        time.sleep(0.1)
        
        if response.status_code != 200:
            print("\nUpload failed.\nServer response: " + response.text)
        else:
            print( "\nUpload successful.");

            
env.Replace(UPLOADCMD=on_upload)
