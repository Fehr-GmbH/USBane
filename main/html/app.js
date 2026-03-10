// USBane - Web Interface JavaScript
// Native USB Execution via WebSocket

let req_count = 0;

// ============================================
// Native WebSocket Communication
// ============================================

let usbSocket = null;
let usbSocketResolve = null;

function getUsbSocket() {
    return new Promise((resolve, reject) => {
        if (usbSocket && usbSocket.readyState === WebSocket.OPEN) {
            resolve(usbSocket);
            return;
        }
        
        const wsUrl = 'ws://' + window.location.host + '/ws/chain';
        usbSocket = new WebSocket(wsUrl);
        
        usbSocket.onopen = () => resolve(usbSocket);
        usbSocket.onerror = (e) => {
            usbSocket = null;
            reject(e);
        };
        usbSocket.onclose = () => {
            usbSocket = null;
        };
    });
}

// Send command and wait for response
function sendNativeCommand(cmd) {
    return new Promise(async (resolve, reject) => {
        try {
            const ws = await getUsbSocket();
            let result = null;  // Store result data from first message
            
            const handler = (event) => {
                try {
                    const data = JSON.parse(event.data);
                    
                    // If this message has result data (i, b, d fields), store it
                    if (data.i !== undefined && data.b !== undefined) {
                        result = data;
                    }
                    
                    // When done/error arrives, resolve with the stored result
                    if (data.status === 'done' || data.status === 'error') {
                        ws.removeEventListener('message', handler);
                        // Return stored result if available, otherwise the done message
                        resolve(result || data);
                    }
                } catch (e) {
                    // Not JSON or parsing error
                }
            };
            
            ws.addEventListener('message', handler);
            ws.send(JSON.stringify(cmd));
            
            // Timeout after 10 seconds
            setTimeout(() => {
                ws.removeEventListener('message', handler);
                reject(new Error('Timeout'));
            }, 10000);
            
        } catch (e) {
            reject(e);
        }
    });
}

// ============================================
// Device Info Tab - Descriptor Fetching & Parsing
// ============================================

let deviceInfoFetching = false;

async function fetchDeviceInfo() {
    if (deviceInfoFetching) return;
    deviceInfoFetching = true;
    
    const btn = document.getElementById('fetchInfoBtn');
    const status = document.getElementById('deviceInfoStatus');
    btn.style.background = '#dc3545';
    btn.innerText = 'Fetching...';
    btn.disabled = true;
    
    try {
        status.innerText = 'Fetching Device Descriptor...';
        status.style.color = '#ff9800';
        
        // 1. Get Device Descriptor (18 bytes)
        const devDesc = await sendDescriptorRequest(0x0100, 0x0000, 18);
        if (devDesc && devDesc.length >= 18) {
            parseDeviceDescriptor(devDesc);
            document.getElementById('devDescSection').style.display = 'block';
            document.getElementById('rawDevDesc').innerHTML = '<span style="color:#888;">Device: </span>' + formatHexBytes(devDesc);
        }
        
        // 2. Get Configuration Descriptor (first get header, then full)
        status.innerText = 'Fetching Configuration Descriptor...';
        const cfgHeader = await sendDescriptorRequest(0x0200, 0x0000, 9);
        if (cfgHeader && cfgHeader.length >= 4) {
            const totalLen = cfgHeader[2] | (cfgHeader[3] << 8);
            const cfgDesc = await sendDescriptorRequest(0x0200, 0x0000, Math.min(totalLen, 512));
            if (cfgDesc && cfgDesc.length >= 9) {
                parseConfigDescriptor(cfgDesc);
                document.getElementById('cfgDescSection').style.display = 'block';
                document.getElementById('rawCfgDesc').innerHTML = '<span style="color:#888;">Config: </span>' + formatHexBytes(cfgDesc);
            }
        }
        
        // 3. Get String Descriptor 0 (Languages)
        status.innerText = 'Fetching String Descriptors...';
        const strLangs = await sendDescriptorRequest(0x0300, 0x0000, 255);
        let langId = 0x0409; // Default to English
        if (strLangs && strLangs.length >= 4) {
            const langs = parseLanguages(strLangs);
            document.getElementById('info_langs').innerText = langs.map(l => '0x' + l.toString(16).padStart(4, '0')).join(', ');
            if (langs.length > 0) langId = langs[0];
        }
        
        // 4. Get String Descriptors 1, 2, 3 (Manufacturer, Product, Serial)
        const strIndices = {
            manufacturer: devDesc ? devDesc[14] : 1,
            product: devDesc ? devDesc[15] : 2,
            serial: devDesc ? devDesc[16] : 3
        };
        
        let rawStrHtml = '';
        
        if (strIndices.manufacturer > 0) {
            const str1 = await sendDescriptorRequest(0x0300 | strIndices.manufacturer, langId, 255);
            if (str1 && str1.length > 2) {
                document.getElementById('info_manufacturer').innerText = parseUsbString(str1);
                rawStrHtml += '<span style="color:#888;">Str' + strIndices.manufacturer + ': </span>' + formatHexBytes(str1) + '<br>';
            }
        }
        
        if (strIndices.product > 0) {
            const str2 = await sendDescriptorRequest(0x0300 | strIndices.product, langId, 255);
            if (str2 && str2.length > 2) {
                document.getElementById('info_product').innerText = parseUsbString(str2);
                rawStrHtml += '<span style="color:#888;">Str' + strIndices.product + ': </span>' + formatHexBytes(str2) + '<br>';
            }
        }
        
        if (strIndices.serial > 0) {
            const str3 = await sendDescriptorRequest(0x0300 | strIndices.serial, langId, 255);
            if (str3 && str3.length > 2) {
                document.getElementById('info_serial').innerText = parseUsbString(str3);
                rawStrHtml += '<span style="color:#888;">Str' + strIndices.serial + ': </span>' + formatHexBytes(str3);
            }
        }
        
        document.getElementById('rawStrDesc').innerHTML = rawStrHtml;
        document.getElementById('strDescSection').style.display = 'block';
        document.getElementById('rawDescSection').style.display = 'block';
        
        status.innerText = 'Device info fetched successfully!';
        status.style.color = '#28a745';
        
    } catch (e) {
        console.error('Error fetching device info:', e);
        status.innerText = 'Error: ' + e.message;
        status.style.color = '#dc3545';
    }
    
    btn.style.background = '#28a745';
    btn.innerText = 'Fetch Device Info';
    btn.disabled = false;
    deviceInfoFetching = false;
}

async function sendDescriptorRequest(wValue, wIndex, wLength) {
    try {
        const result = await sendNativeCommand({
            cmd: 'single',
            bmRequestType: '0x80',
            bRequest: '0x06',
            wValue: '0x' + wValue.toString(16).padStart(4, '0'),
            wIndex: '0x' + wIndex.toString(16).padStart(4, '0'),
            wLength: wLength,
            packetSize: 8,
            timeout: 1000
        });
        
        if (result.d && result.b > 0) {
            // Parse hex string to byte array
            const hexStr = result.d.replace(/\s/g, '');
            const bytes = [];
            for (let i = 0; i < hexStr.length; i += 2) {
                bytes.push(parseInt(hexStr.substr(i, 2), 16));
            }
            return bytes;
        }
    } catch (e) {
        console.error('Descriptor request failed:', e);
    }
    return null;
}

function parseDeviceDescriptor(data) {
    if (data.length < 18) return;
    
    const bcdUSB = data[2] | (data[3] << 8);
    const bcdDevice = data[12] | (data[13] << 8);
    const idVendor = data[8] | (data[9] << 8);
    const idProduct = data[10] | (data[11] << 8);
    
    document.getElementById('info_usb_ver').innerText = formatBCD(bcdUSB);
    document.getElementById('info_vid').innerText = '0x' + idVendor.toString(16).padStart(4, '0').toUpperCase();
    document.getElementById('info_pid').innerText = '0x' + idProduct.toString(16).padStart(4, '0').toUpperCase();
    document.getElementById('info_dev_ver').innerText = formatBCD(bcdDevice);
    document.getElementById('info_class').innerText = formatDeviceClass(data[4]);
    document.getElementById('info_subclass').innerText = '0x' + data[5].toString(16).padStart(2, '0');
    document.getElementById('info_protocol').innerText = '0x' + data[6].toString(16).padStart(2, '0');
    document.getElementById('info_mps').innerText = data[7] + ' bytes';
    document.getElementById('info_num_cfg').innerText = data[17];
}

function parseConfigDescriptor(data) {
    if (data.length < 9) return;
    
    const wTotalLength = data[2] | (data[3] << 8);
    const bNumInterfaces = data[4];
    const bConfigurationValue = data[5];
    const bmAttributes = data[7];
    const bMaxPower = data[8];
    
    document.getElementById('info_cfg_len').innerText = wTotalLength + ' bytes';
    document.getElementById('info_num_if').innerText = bNumInterfaces;
    document.getElementById('info_cfg_val').innerText = bConfigurationValue;
    document.getElementById('info_cfg_attr').innerText = formatConfigAttributes(bmAttributes);
    document.getElementById('info_max_pwr').innerText = (bMaxPower * 2) + ' mA';
    
    // Parse interfaces and endpoints
    const interfacesDiv = document.getElementById('interfacesSection');
    interfacesDiv.innerHTML = '';
    
    let offset = 9;
    let currentInterface = null;
    
    while (offset < data.length) {
        const bLength = data[offset];
        const bDescriptorType = data[offset + 1];
        
        if (bLength < 2 || offset + bLength > data.length) break;
        
        if (bDescriptorType === 4 && bLength >= 9) {
            // Interface Descriptor
            const ifNum = data[offset + 2];
            const altSetting = data[offset + 3];
            const numEndpoints = data[offset + 4];
            const ifClass = data[offset + 5];
            const ifSubclass = data[offset + 6];
            const ifProtocol = data[offset + 7];
            
            currentInterface = document.createElement('div');
            currentInterface.className = 'interface-card';
            currentInterface.innerHTML = `
                <h4>Interface ${ifNum} (Alt ${altSetting})</h4>
                <div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;font-size:12px;">
                    <span><span style="color:#888;">Class:</span> ${formatInterfaceClass(ifClass)}</span>
                    <span><span style="color:#888;">Subclass:</span> 0x${ifSubclass.toString(16).padStart(2,'0')}</span>
                    <span><span style="color:#888;">Protocol:</span> 0x${ifProtocol.toString(16).padStart(2,'0')}</span>
                    <span><span style="color:#888;">Endpoints:</span> ${numEndpoints}</span>
                </div>
            `;
            interfacesDiv.appendChild(currentInterface);
        }
        else if (bDescriptorType === 5 && bLength >= 7 && currentInterface) {
            // Endpoint Descriptor
            const epAddr = data[offset + 2];
            const epAttr = data[offset + 3];
            const epMaxPktSize = data[offset + 4] | (data[offset + 5] << 8);
            const epInterval = data[offset + 6];
            
            const epDir = (epAddr & 0x80) ? 'IN' : 'OUT';
            const epNum = epAddr & 0x0F;
            const epType = ['Control', 'Isochronous', 'Bulk', 'Interrupt'][epAttr & 0x03];
            
            const epDiv = document.createElement('div');
            epDiv.className = 'endpoint-item';
            epDiv.innerHTML = `<span style="color:#ffb74d;">EP${epNum} ${epDir}</span> &nbsp;|&nbsp; ${epType} &nbsp;|&nbsp; Max: ${epMaxPktSize} bytes &nbsp;|&nbsp; Interval: ${epInterval}ms`;
            currentInterface.appendChild(epDiv);
        }
        
        offset += bLength;
    }
}

function parseLanguages(data) {
    const langs = [];
    if (data.length >= 4) {
        for (let i = 2; i < data[0] && i + 1 < data.length; i += 2) {
            langs.push(data[i] | (data[i + 1] << 8));
        }
    }
    return langs;
}

function parseUsbString(data) {
    if (data.length < 2) return '';
    const len = Math.min(data[0], data.length);
    let str = '';
    for (let i = 2; i < len; i += 2) {
        const charCode = data[i] | (data[i + 1] << 8);
        if (charCode > 0 && charCode < 0x10000) {
            str += String.fromCharCode(charCode);
        }
    }
    return str;
}

function formatBCD(bcd) {
    const major = (bcd >> 8) & 0xFF;
    const minor = (bcd >> 4) & 0x0F;
    const patch = bcd & 0x0F;
    return `${major}.${minor}${patch > 0 ? '.' + patch : ''}`;
}

function formatDeviceClass(classCode) {
    const classes = {
        0x00: '0x00 (Per-Interface)',
        0x01: '0x01 (Audio)',
        0x02: '0x02 (CDC)',
        0x03: '0x03 (HID)',
        0x05: '0x05 (Physical)',
        0x06: '0x06 (Image)',
        0x07: '0x07 (Printer)',
        0x08: '0x08 (Mass Storage)',
        0x09: '0x09 (Hub)',
        0x0A: '0x0A (CDC-Data)',
        0x0B: '0x0B (Smart Card)',
        0x0D: '0x0D (Content Security)',
        0x0E: '0x0E (Video)',
        0x0F: '0x0F (Personal Healthcare)',
        0xDC: '0xDC (Diagnostic)',
        0xE0: '0xE0 (Wireless)',
        0xEF: '0xEF (Misc)',
        0xFE: '0xFE (Application-Specific)',
        0xFF: '0xFF (Vendor-Specific)'
    };
    return classes[classCode] || '0x' + classCode.toString(16).padStart(2, '0');
}

function formatInterfaceClass(classCode) {
    const classes = {
        0x01: 'Audio',
        0x02: 'CDC',
        0x03: 'HID',
        0x05: 'Physical',
        0x06: 'Image',
        0x07: 'Printer',
        0x08: 'Mass Storage',
        0x09: 'Hub',
        0x0A: 'CDC-Data',
        0x0B: 'Smart Card',
        0x0D: 'Content Security',
        0x0E: 'Video',
        0x0F: 'Healthcare',
        0xFE: 'App-Specific',
        0xFF: 'Vendor'
    };
    return classes[classCode] || '0x' + classCode.toString(16).padStart(2, '0');
}

function formatConfigAttributes(attr) {
    const parts = [];
    if (attr & 0x40) parts.push('Self-Powered');
    if (attr & 0x20) parts.push('Remote Wakeup');
    if (parts.length === 0) parts.push('Bus-Powered');
    return parts.join(', ');
}

function formatHexBytes(bytes) {
    return bytes.map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
}

// ============================================
// End Device Info Tab
// ============================================

// ============================================
// Request Chain Tab
// ============================================

let chainRequests = [];
let chainRunning = false;
let chainCurrentIndex = 0;
let chainWaitingForContinue = false;
let chainContinueResolver = null;
let chainWs = null;  // WebSocket for native chain execution

function addToChain() {
    const req = {
        type: 'control',  // Action type: control, bulk_in, bulk_out, interrupt_in, interrupt_out, iso_in, iso_out, waitfor, action, condition
        bmRequestType: document.getElementById('bmRequestType').value,
        bRequest: document.getElementById('bRequest').value,
        wValue: document.getElementById('wValue').value,
        wIndex: document.getElementById('wIndex').value,
        wLength: parseInt(document.getElementById('wLength').value),
        packetSize: parseInt(document.getElementById('packetSize').value),
        dataMode: document.getElementById('dataMode').value,
        dataBytes: document.getElementById('dataBytes').value
    };
    
    chainRequests.push(req);
    renderChain();
    
    // Visual feedback
    const btn = event.target;
    const origText = btn.innerText;
    btn.innerText = 'Added!';
    btn.style.background = '#28a745';
    setTimeout(() => {
        btn.innerText = origText;
        btn.style.background = '#17a2b8';
    }, 500);
}

function selectChainQuickDataMode(mode) {
    document.getElementById('chain_quick_dataMode').value = mode;
    if (mode === 'separate') {
        document.getElementById('chain_quick_dataMode_separate').style.background = '#dc3545';
        document.getElementById('chain_quick_dataMode_separate').style.color = '#fff';
        document.getElementById('chain_quick_dataMode_separate').style.fontWeight = 'bold';
        document.getElementById('chain_quick_dataMode_append').style.background = '#333';
        document.getElementById('chain_quick_dataMode_append').style.color = '#888';
        document.getElementById('chain_quick_dataMode_append').style.fontWeight = 'normal';
    } else {
        document.getElementById('chain_quick_dataMode_append').style.background = '#dc3545';
        document.getElementById('chain_quick_dataMode_append').style.color = '#fff';
        document.getElementById('chain_quick_dataMode_append').style.fontWeight = 'bold';
        document.getElementById('chain_quick_dataMode_separate').style.background = '#333';
        document.getElementById('chain_quick_dataMode_separate').style.color = '#888';
        document.getElementById('chain_quick_dataMode_separate').style.fontWeight = 'normal';
    }
}

function selectChainQuickRetry(retry) {
    document.getElementById('chain_quick_retry').value = retry ? 'true' : 'false';
    if (retry) {
        document.getElementById('chain_quick_retry_yes').style.background = '#dc3545';
        document.getElementById('chain_quick_retry_yes').style.color = '#fff';
        document.getElementById('chain_quick_retry_yes').style.fontWeight = 'bold';
        document.getElementById('chain_quick_retry_no').style.background = '#333';
        document.getElementById('chain_quick_retry_no').style.color = '#888';
        document.getElementById('chain_quick_retry_no').style.fontWeight = 'normal';
    } else {
        document.getElementById('chain_quick_retry_no').style.background = '#dc3545';
        document.getElementById('chain_quick_retry_no').style.color = '#fff';
        document.getElementById('chain_quick_retry_no').style.fontWeight = 'bold';
        document.getElementById('chain_quick_retry_yes').style.background = '#333';
        document.getElementById('chain_quick_retry_yes').style.color = '#888';
        document.getElementById('chain_quick_retry_yes').style.fontWeight = 'normal';
    }
}

function addQuickChainRequest() {
    const retryValue = document.getElementById('chain_quick_retry').value;
    const req = {
        type: 'control',
        bmRequestType: document.getElementById('chain_quick_bmRequestType').value,
        bRequest: document.getElementById('chain_quick_bRequest').value,
        wValue: document.getElementById('chain_quick_wValue').value,
        wIndex: document.getElementById('chain_quick_wIndex').value,
        wLength: parseInt(document.getElementById('chain_quick_wLength').value),
        packetSize: parseInt(document.getElementById('chain_quick_packetSize').value),
        dataMode: document.getElementById('chain_quick_dataMode').value,
        dataBytes: document.getElementById('chain_quick_data').value,
        noRetry: retryValue === 'false',  // noRetry=true when Retry=NO
        deviceAddr: parseInt(document.getElementById('chain_quick_deviceAddr').value) || 0,
        dataStageEp: parseInt(document.getElementById('chain_quick_dataStageEp').value) || 0,
        setupOnly: document.getElementById('chain_quick_setupOnly').value === 'true'
    };
    
    chainRequests.push(req);
    renderChain();
    
    // Visual feedback
    const btn = event.target;
    const origText = btn.innerText;
    btn.innerText = '✓';
    btn.style.background = '#17a2b8';
    setTimeout(() => {
        btn.innerText = origText;
        btn.style.background = '#17a2b8';
    }, 500);
}

// Chain Quick SetupOnly toggle
function selectChainQuickSetupOnly(value) {
    document.getElementById('chain_quick_setupOnly').value = value ? 'true' : 'false';
    const noBtn = document.getElementById('chain_quick_setupOnly_no');
    const yesBtn = document.getElementById('chain_quick_setupOnly_yes');
    
    if (value) {
        yesBtn.style.background = '#dc3545';
        yesBtn.style.color = '#fff';
        yesBtn.style.fontWeight = 'bold';
        noBtn.style.background = '#333';
        noBtn.style.color = '#888';
        noBtn.style.fontWeight = 'normal';
    } else {
        noBtn.style.background = '#dc3545';
        noBtn.style.color = '#fff';
        noBtn.style.fontWeight = 'bold';
        yesBtn.style.background = '#333';
        yesBtn.style.color = '#888';
        yesBtn.style.fontWeight = 'normal';
    }
}

// Chain Endpoint (Bulk/Interrupt) functions
function selectChainEpDir(dir) {
    document.getElementById('chain_ep_direction').value = dir;
    const inBtn = document.getElementById('chain_ep_dir_in');
    const outBtn = document.getElementById('chain_ep_dir_out');
    const dataInput = document.getElementById('chain_ep_data');
    
    if (dir === 'in') {
        inBtn.style.background = '#17a2b8';
        inBtn.style.color = '#fff';
        inBtn.style.fontWeight = 'bold';
        outBtn.style.background = '#333';
        outBtn.style.color = '#888';
        outBtn.style.fontWeight = 'normal';
        dataInput.disabled = true;
        dataInput.style.background = '#1a1a1a';
        dataInput.placeholder = 'Not used for IN';
    } else {
        outBtn.style.background = '#17a2b8';
        outBtn.style.color = '#fff';
        outBtn.style.fontWeight = 'bold';
        inBtn.style.background = '#333';
        inBtn.style.color = '#888';
        inBtn.style.fontWeight = 'normal';
        dataInput.disabled = false;
        dataInput.style.background = '#333';
        dataInput.placeholder = 'hex bytes to send';
    }
}

function selectChainEpType(type) {
    document.getElementById('chain_ep_type').value = type;
    const bulkBtn = document.getElementById('chain_ep_type_bulk');
    const intBtn = document.getElementById('chain_ep_type_int');
    const isoBtn = document.getElementById('chain_ep_type_iso');
    
    if (type === 'bulk') {
        bulkBtn.style.background = '#17a2b8';
        bulkBtn.style.color = '#fff';
        bulkBtn.style.fontWeight = 'bold';
        intBtn.style.background = '#333';
        intBtn.style.color = '#888';
        intBtn.style.fontWeight = 'normal';
        if (isoBtn) {
            isoBtn.style.background = '#333';
            isoBtn.style.color = '#888';
            isoBtn.style.fontWeight = 'normal';
        }
    } else if (type === 'iso') {
        if (isoBtn) {
            isoBtn.style.background = '#17a2b8';
            isoBtn.style.color = '#fff';
            isoBtn.style.fontWeight = 'bold';
        }
        bulkBtn.style.background = '#333';
        bulkBtn.style.color = '#888';
        bulkBtn.style.fontWeight = 'normal';
        intBtn.style.background = '#333';
        intBtn.style.color = '#888';
        intBtn.style.fontWeight = 'normal';
    } else {
        intBtn.style.background = '#17a2b8';
        intBtn.style.color = '#fff';
        intBtn.style.fontWeight = 'bold';
        bulkBtn.style.background = '#333';
        bulkBtn.style.color = '#888';
        bulkBtn.style.fontWeight = 'normal';
        if (isoBtn) {
            isoBtn.style.background = '#333';
            isoBtn.style.color = '#888';
            isoBtn.style.fontWeight = 'normal';
        }
    }
}

function selectChainEpCont(cont) {
    document.getElementById('chain_ep_continuous').value = cont ? 'true' : 'false';
    const noBtn = document.getElementById('chain_ep_cont_no');
    const yesBtn = document.getElementById('chain_ep_cont_yes');
    
    if (cont) {
        yesBtn.style.background = '#17a2b8';
        yesBtn.style.color = '#fff';
        yesBtn.style.fontWeight = 'bold';
        noBtn.style.background = '#333';
        noBtn.style.color = '#888';
        noBtn.style.fontWeight = 'normal';
    } else {
        noBtn.style.background = '#17a2b8';
        noBtn.style.color = '#fff';
        noBtn.style.fontWeight = 'bold';
        yesBtn.style.background = '#333';
        yesBtn.style.color = '#888';
        yesBtn.style.fontWeight = 'normal';
    }
}

function addQuickChainEndpoint() {
    const direction = document.getElementById('chain_ep_direction').value;
    const epType = document.getElementById('chain_ep_type').value;
    
    // Determine type: bulk_in, bulk_out, interrupt_in, interrupt_out, iso_in, iso_out
    let reqType;
    if (epType === 'bulk') {
        reqType = direction === 'in' ? 'bulk_in' : 'bulk_out';
    } else if (epType === 'iso') {
        reqType = direction === 'in' ? 'iso_in' : 'iso_out';
    } else {
        reqType = direction === 'in' ? 'interrupt_in' : 'interrupt_out';
    }
    
    const req = {
        type: reqType,
        endpoint: parseInt(document.getElementById('chain_ep_endpoint').value) || 1,
        deviceAddr: parseInt(document.getElementById('chain_ep_addr').value) || 0,
        length: parseInt(document.getElementById('chain_ep_length').value) || 8,
        timeout: parseInt(document.getElementById('chain_ep_timeout').value) || 1000,
        continuous: document.getElementById('chain_ep_continuous').value === 'true',
        maxAttempts: parseInt(document.getElementById('chain_ep_max_attempts').value) || 1,
        channel: 1  // Default channel
    };
    
    // Add data for OUT transfers
    if (direction === 'out') {
        req.dataBytes = document.getElementById('chain_ep_data').value.replace(/\s/g, '');
    }
    
    chainRequests.push(req);
    renderChain();
    
    // Visual feedback
    const btn = event.target;
    const origText = btn.innerText;
    btn.innerText = '✓';
    btn.style.background = '#17a2b8';
    setTimeout(() => {
        btn.innerText = origText;
        btn.style.background = '#17a2b8';
    }, 500);
}

function selectWaitType(type) {
    document.getElementById('wait_current_type').value = type;
    
    // Update toggle visuals - all use yellow (#ffc107) for wait area
    ['button', 'webhook', 'gpio', 'usb_reset', 'delay'].forEach(t => {
        const elem = document.getElementById('wait_type_' + t);
        if (t === type) {
            elem.style.background = '#ffc107';
            elem.style.color = '#fff';
            elem.style.fontWeight = 'bold';
        } else {
            elem.style.background = '#333';
            elem.style.color = '#888';
            elem.style.fontWeight = 'normal';
        }
    });
    
    // Update fields
    const fieldsDiv = document.getElementById('wait_fields');
    if (type === 'button') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Label</label>
                <input type='text' id='wait_button_label' value='Press Continue to proceed' placeholder='Button label' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    } else if (type === 'webhook') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Trigger ID</label>
                <input type='text' id='wait_webhook_id' value='trigger1' placeholder='Webhook ID' style='padding:6px;font-size:11px;width:100%;'>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Timeout (seconds)</label>
                <input type='number' id='wait_webhook_timeout' value='300' min='1' max='3600' placeholder='300' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    } else if (type === 'gpio') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>GPIO Pin (0-48)</label>
                <input type='number' id='wait_gpio_pin' value='0' min='0' max='48' placeholder='0' style='padding:6px;font-size:11px;width:100%;'>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Trigger Level</label>
                <div style='display:flex;border:1px solid #555;border-radius:4px;overflow:hidden;'>
                    <div id='wait_gpio_high' onclick='selectWaitGpioLevel("high")' style='flex:1;padding:6px;text-align:center;cursor:pointer;background:#ffc107;color:#fff;font-weight:bold;transition:all 0.2s;font-size:9px;'>HIGH</div>
                    <div id='wait_gpio_low' onclick='selectWaitGpioLevel("low")' style='flex:1;padding:6px;text-align:center;cursor:pointer;background:#333;color:#888;transition:all 0.2s;font-size:9px;'>LOW</div>
                </div>
                <input type='hidden' id='wait_gpio_level' value='high'>
            </div>
        `;
    } else if (type === 'usb_reset') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Note</label>
                <input type='text' id='wait_reset_note' value='USB Reset' placeholder='Optional note' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    } else if (type === 'delay') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Duration (ms)</label>
                <input type='number' id='wait_delay_duration' value='1000' min='1' max='60000' placeholder='1000' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    }
}

function selectWaitGpioLevel(level) {
    document.getElementById('wait_gpio_level').value = level;
    document.getElementById('wait_gpio_high').style.background = level === 'high' ? '#ffc107' : '#333';
    document.getElementById('wait_gpio_high').style.color = level === 'high' ? '#fff' : '#888';
    document.getElementById('wait_gpio_high').style.fontWeight = level === 'high' ? 'bold' : 'normal';
    document.getElementById('wait_gpio_low').style.background = level === 'low' ? '#ffc107' : '#333';
    document.getElementById('wait_gpio_low').style.color = level === 'low' ? '#fff' : '#888';
    document.getElementById('wait_gpio_low').style.fontWeight = level === 'low' ? 'bold' : 'normal';
}

function selectActionType(type) {
    document.getElementById('action_current_type').value = type;
    
    // Update toggle visuals - all use green (#28a745) for action area
    ['http', 'gpio_out', 'comment', 'copy', 'goto'].forEach(t => {
        const elem = document.getElementById('action_type_' + t);
        if (t === type) {
            elem.style.background = '#28a745';
            elem.style.color = '#fff';
            elem.style.fontWeight = 'bold';
        } else {
            elem.style.background = '#333';
            elem.style.color = '#888';
            elem.style.fontWeight = 'normal';
        }
    });
    
    // Update fields
    const fieldsDiv = document.getElementById('action_fields');
    if (type === 'http') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>URL</label>
                <input type='text' id='action_http_url' value='http://example.com/api' placeholder='http://...' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    } else if (type === 'gpio_out') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>GPIO Pin (0-48)</label>
                <input type='number' id='action_gpio_out_pin' value='0' min='0' max='48' placeholder='0' style='padding:6px;font-size:11px;width:100%;'>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Output Level</label>
                <div style='display:flex;border:1px solid #555;border-radius:4px;overflow:hidden;'>
                    <div id='action_gpio_out_high' onclick='selectActionGpioLevel("high")' style='flex:1;padding:6px;text-align:center;cursor:pointer;background:#28a745;color:#fff;font-weight:bold;transition:all 0.2s;font-size:9px;'>HIGH</div>
                    <div id='action_gpio_out_low' onclick='selectActionGpioLevel("low")' style='flex:1;padding:6px;text-align:center;cursor:pointer;background:#333;color:#888;transition:all 0.2s;font-size:9px;'>LOW</div>
                </div>
                <input type='hidden' id='action_gpio_out_level' value='high'>
            </div>
        `;
    } else if (type === 'comment') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Comment Text</label>
                <input type='text' id='action_comment_text' value='' placeholder='Add a comment...' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    } else if (type === 'copy') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Copy From</label>
                <select id='action_copy_source' style='padding:6px;font-size:11px;width:100%;'>
                    <option value='rxbytes'>RxBytes</option>
                    <option value='responsehex' selected>Response Hex</option>
                </select>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>From ReqNo</label>
                <input type='number' id='action_copy_from_reqno' value='-1' placeholder='-1=last' style='padding:6px;font-size:11px;width:100%;'>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>From Offset</label>
                <input type='number' id='action_copy_from_offset' value='0' min='0' placeholder='0' style='padding:6px;font-size:11px;width:100%;'>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>From Length</label>
                <input type='number' id='action_copy_from_length' value='-1' placeholder='-1=all' style='padding:6px;font-size:11px;width:100%;'>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Paste To Field</label>
                <select id='action_copy_dest_field' style='padding:6px;font-size:11px;width:100%;'>
                    <option value='bmRequestType'>bmRequestType</option>
                    <option value='bRequest'>bRequest</option>
                    <option value='wValue' selected>wValue</option>
                    <option value='wIndex'>wIndex</option>
                    <option value='wLength'>wLength</option>
                    <option value='packetSize'>packetSize</option>
                    <option value='dataBytes'>dataBytes</option>
                    <option value='endpoint'>endpoint</option>
                    <option value='deviceAddr'>deviceAddr</option>
                </select>
            </div>
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>To ReqNo</label>
                <input type='number' id='action_copy_to_reqno' value='-1' placeholder='-1=next' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    } else if (type === 'goto') {
        fieldsDiv.innerHTML = `
            <div style='flex:1;'>
                <label style='display:block;font-size:9px;color:#888;margin-bottom:3px;'>Go To ReqNo (0-based index)</label>
                <input type='number' id='action_goto_reqno' value='0' min='0' placeholder='0' style='padding:6px;font-size:11px;width:100%;'>
            </div>
        `;
    }
}

function selectActionGpioLevel(level) {
    document.getElementById('action_gpio_out_level').value = level;
    document.getElementById('action_gpio_out_high').style.background = level === 'high' ? '#28a745' : '#333';
    document.getElementById('action_gpio_out_high').style.color = level === 'high' ? '#fff' : '#888';
    document.getElementById('action_gpio_out_high').style.fontWeight = level === 'high' ? 'bold' : 'normal';
    document.getElementById('action_gpio_out_low').style.background = level === 'low' ? '#28a745' : '#333';
    document.getElementById('action_gpio_out_low').style.color = level === 'low' ? '#fff' : '#888';
    document.getElementById('action_gpio_out_low').style.fontWeight = level === 'low' ? 'bold' : 'normal';
}

function addQuickWait() {
    const type = document.getElementById('wait_current_type').value;
    let wait = {};
    
    if (type === 'button') {
        wait = {
            type: 'waitfor',
            waitType: 'button',
            label: document.getElementById('wait_button_label').value
        };
    } else if (type === 'webhook') {
        wait = {
            type: 'waitfor',
            waitType: 'webhook',
            triggerId: document.getElementById('wait_webhook_id').value,
            timeout: parseInt(document.getElementById('wait_webhook_timeout').value)
        };
    } else if (type === 'gpio') {
        const level = document.getElementById('wait_gpio_level').value;
        wait = {
            type: 'waitfor',
            waitType: 'gpio',
            gpioPin: parseInt(document.getElementById('wait_gpio_pin').value),
            gpioLevel: level === 'high' ? 1 : 0,
            timeout: 60
        };
    } else if (type === 'usb_reset') {
        wait = {
            type: 'waitfor',
            waitType: 'usb_reset',
            note: document.getElementById('wait_reset_note').value
        };
    } else if (type === 'delay') {
        wait = {
            type: 'waitfor',
            waitType: 'delay',
            duration: parseInt(document.getElementById('wait_delay_duration').value)
        };
    }
    
    chainRequests.push(wait);
    renderChain();
    
    // Visual feedback
    const btn = event.target;
    const origText = btn.innerText;
    btn.innerText = '✓';
    setTimeout(() => {
        btn.innerText = origText;
    }, 500);
}

function addQuickAction() {
    const type = document.getElementById('action_current_type').value;
    let action = {};
    
    if (type === 'http') {
        action = {
            type: 'action',
            actionType: 'http',
            url: document.getElementById('action_http_url').value
        };
    } else if (type === 'gpio_out') {
        const level = document.getElementById('action_gpio_out_level').value;
        action = {
            type: 'action',
            actionType: 'gpio_out',
            gpioPin: parseInt(document.getElementById('action_gpio_out_pin').value),
            gpioLevel: level === 'high' ? 1 : 0
        };
    } else if (type === 'comment') {
        action = {
            type: 'action',
            actionType: 'comment',
            text: document.getElementById('action_comment_text').value || 'Comment'
        };
    } else if (type === 'copy') {
        action = {
            type: 'action',
            actionType: 'copy',
            copyFromSource: document.getElementById('action_copy_source').value,
            copyFromReqNo: parseInt(document.getElementById('action_copy_from_reqno').value) || -1,
            copyFromOffset: parseInt(document.getElementById('action_copy_from_offset').value) || 0,
            copyFromLength: parseInt(document.getElementById('action_copy_from_length').value) || -1,
            copyToField: document.getElementById('action_copy_dest_field').value,
            copyToReqNo: parseInt(document.getElementById('action_copy_to_reqno').value) || -1
        };
    } else if (type === 'goto') {
        action = {
            type: 'action',
            actionType: 'goto',
            gotoReqNo: parseInt(document.getElementById('action_goto_reqno').value) || 0
        };
    }
    
    chainRequests.push(action);
    renderChain();
    
    // Visual feedback
    const btn = event.target;
    const origText = btn.innerText;
    btn.innerText = '✓';
    btn.style.background = '#17a2b8';
    setTimeout(() => {
        btn.innerText = origText;
    }, 500);
}

function selectConditionSourceA() {
    const source = document.getElementById('condition_value_a_source').value;
    if (source === 'rxbytes' || source === 'responsehex') {
        document.getElementById('condition_a_reqno_div').style.display = 'block';
        document.getElementById('condition_a_length_div').style.display = 'block';
        document.getElementById('condition_a_value_div').style.display = 'none';
    } else {
        document.getElementById('condition_a_reqno_div').style.display = 'none';
        document.getElementById('condition_a_length_div').style.display = 'none';
        document.getElementById('condition_a_value_div').style.display = 'block';
    }
}

function selectConditionSourceB() {
    const source = document.getElementById('condition_value_b_source').value;
    if (source === 'rxbytes' || source === 'responsehex') {
        document.getElementById('condition_b_reqno_div').style.display = 'block';
        document.getElementById('condition_b_length_div').style.display = 'block';
        document.getElementById('condition_b_value_div').style.display = 'none';
    } else {
        document.getElementById('condition_b_reqno_div').style.display = 'none';
        document.getElementById('condition_b_length_div').style.display = 'none';
        document.getElementById('condition_b_value_div').style.display = 'block';
    }
}

function addQuickCondition() {
    const condition = {
        type: 'condition',
        operator: document.getElementById('condition_operator').value,
        valueASource: document.getElementById('condition_value_a_source').value,
        valueAReqNo: parseInt(document.getElementById('condition_a_reqno').value) || -1,
        valueALength: parseInt(document.getElementById('condition_a_length').value) || 1,
        valueAValue: document.getElementById('condition_a_value').value || '',
        valueBSource: document.getElementById('condition_value_b_source').value,
        valueBReqNo: parseInt(document.getElementById('condition_b_reqno').value) || -1,
        valueBLength: parseInt(document.getElementById('condition_b_length').value) || 1,
        valueBValue: document.getElementById('condition_b_value').value || '',
        action: document.getElementById('condition_action').value
    };
    
    chainRequests.push(condition);
    renderChain();
    
    // Visual feedback
    const btn = event.target;
    const origText = btn.innerText;
    btn.innerText = '✓';
    btn.style.background = '#17a2b8';
    setTimeout(() => {
        btn.innerText = origText;
    }, 500);
}

function addWaitforButton() {
    chainRequests.push({
        type: 'waitfor',
        waitType: 'button',
        label: 'Press Continue to proceed'
    });
    renderChain();
}

function addWaitforWebhook() {
    const triggerId = prompt('Enter trigger ID (will be used in webhook URL):', 'trigger1');
    if (!triggerId) return;
    
    chainRequests.push({
        type: 'waitfor',
        waitType: 'webhook',
        triggerId: triggerId,
        timeout: 300  // 5 minute default timeout
    });
    renderChain();
}

function addWaitforGpio() {
    const pin = prompt('Enter GPIO pin number:', '0');
    if (pin === null) return;
    const pinNum = parseInt(pin);
    if (isNaN(pinNum) || pinNum < 0 || pinNum > 48) {
        alert('Invalid GPIO pin (0-48)');
        return;
    }
    
    const triggerLevel = prompt('Trigger when HIGH or LOW?', 'HIGH').toUpperCase();
    if (triggerLevel !== 'HIGH' && triggerLevel !== 'LOW') {
        alert('Invalid level, use HIGH or LOW');
        return;
    }
    
    chainRequests.push({
        type: 'waitfor',
        waitType: 'gpio',
        gpioPin: pinNum,
        gpioLevel: triggerLevel === 'HIGH' ? 1 : 0,
        timeout: 60  // 1 minute default timeout
    });
    renderChain();
}

function removeFromChain(index) {
    chainRequests.splice(index, 1);
    renderChain();
}

function moveChainItem(index, direction) {
    const newIndex = index + direction;
    if (newIndex < 0 || newIndex >= chainRequests.length) return;
    
    const item = chainRequests[index];
    chainRequests.splice(index, 1);
    chainRequests.splice(newIndex, 0, item);
    renderChain();
}

function clearChain() {
    if (chainRequests.length === 0) return;
    if (!confirm('Clear all ' + chainRequests.length + ' requests from chain?')) return;
    chainRequests = [];
    renderChain();
}

function renderChain() {
    const emptyDiv = document.getElementById('chainEmpty');
    const table = document.getElementById('chainTable');
    const tbody = document.getElementById('chainTableBody');
    
    if (chainRequests.length === 0) {
        emptyDiv.style.display = 'block';
        table.style.display = 'none';
        return;
    }
    
    emptyDiv.style.display = 'none';
    table.style.display = 'table';
    
    let html = '';
    chainRequests.forEach((req, i) => {
        const actionType = req.type || 'control';
        
        if (actionType === 'waitfor') {
            // Waitfor action row - yellow area color (#ffc107)
            let bgColor = '#ffc107';
            let waitLabel, configInfo;
            if (req.waitType === 'button') {
                waitLabel = 'WAIT: Button';
                configInfo = req.label || 'Press Continue';
            } else if (req.waitType === 'gpio') {
                waitLabel = 'WAIT: GPIO';
                configInfo = 'Pin ' + req.gpioPin + ' = ' + (req.gpioLevel ? 'HIGH' : 'LOW');
            } else if (req.waitType === 'delay') {
                waitLabel = 'WAIT: Delay';
                configInfo = (req.duration || 0) + ' ms';
            } else if (req.waitType === 'usb_reset') {
                waitLabel = 'WAIT: USB Reset';
                configInfo = req.note || 'USB Reset';
            } else if (req.waitType === 'vbus_cycle') {
                waitLabel = 'WAIT: VBUS Cycle';
                configInfo = 'Full power cycle (VBUS off/on + reset)';
            } else {
                waitLabel = 'WAIT: Webhook';
                configInfo = 'ID: ' + (req.triggerId || 'N/A');
            }
            
            html += '<tr style="background:' + bgColor + '22;">';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">' + (i + 1) + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;color:' + bgColor + ';font-weight:bold;">' + waitLabel + '</td>';
            html += '<td colspan="7" style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editWaitforConfig(' + i + ',this)">' + configInfo + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">';
            html += '<button onclick="moveChainItem(' + i + ',-1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">^</button>';
            html += '<button onclick="moveChainItem(' + i + ',1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">v</button>';
            html += '<button onclick="removeFromChain(' + i + ')" style="padding:2px 6px;margin:1px;font-size:10px;background:#dc3545;">x</button>';
            html += '</td>';
            html += '</tr>';
        } else if (actionType === 'action') {
            // Action row - green area color (#28a745)
            let bgColor = '#28a745';
            let actionLabel, configInfo;
            if (req.actionType === 'http') {
                actionLabel = 'HTTP';
                configInfo = req.url || 'No URL';
            } else if (req.actionType === 'gpio_out') {
                actionLabel = 'GPIO';
                configInfo = 'Pin ' + req.gpioPin + ' -> ' + (req.gpioLevel ? 'HIGH' : 'LOW');
            } else if (req.actionType === 'comment') {
                actionLabel = 'COMMENT';
                configInfo = req.text || 'No comment';
            } else if (req.actionType === 'copy' || req.actionType === 'copy_to') {
                actionLabel = 'COPY';
                const fromReqNo = req.copyFromReqNo === -1 ? 'last' : (req.copyFromReqNo < 0 ? 'last' + req.copyFromReqNo : '#' + req.copyFromReqNo);
                const fromOffset = req.copyFromOffset || 0;
                const fromLen = req.copyFromLength === -1 ? 'all' : req.copyFromLength + 'B';
                const toReqNo = req.copyToReqNo === -1 ? 'next' : (req.copyToReqNo < 0 ? 'next' + (req.copyToReqNo + 1) : '#' + req.copyToReqNo);
                const offsetStr = fromOffset > 0 ? '@' + fromOffset + ':' : '';
                configInfo = 'Req' + fromReqNo + '[' + offsetStr + fromLen + '] -> ' + req.copyToField + ' of Req' + toReqNo;
            } else if (req.actionType === 'goto') {
                actionLabel = 'GOTO';
                configInfo = 'Jump to index ' + (req.gotoReqNo || 0);
            } else if (req.actionType === 'add32') {
                actionLabel = 'ADD32';
                var fieldName = req.add32Field || 'wValue';
                configInfo = '+' + (req.add32Increment || '0x40') + ' → ' + fieldName + ' of entries [' + (req.add32Entries || []).join(',') + ']';
            } else {
                actionLabel = 'ACTION';
                configInfo = 'Unknown';
            }
            
            html += '<tr style="background:' + bgColor + '22;">';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">' + (i + 1) + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;color:' + bgColor + ';font-weight:bold;">' + actionLabel + '</td>';
            html += '<td colspan="7" style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editActionConfig(' + i + ',this)">' + configInfo + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">';
            html += '<button onclick="moveChainItem(' + i + ',-1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">^</button>';
            html += '<button onclick="moveChainItem(' + i + ',1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">v</button>';
            html += '<button onclick="removeFromChain(' + i + ')" style="padding:2px 6px;margin:1px;font-size:10px;background:#dc3545;">x</button>';
            html += '</td>';
            html += '</tr>';
        } else if (actionType === 'bulk_in' || actionType === 'interrupt_in' || actionType === 'iso_in') {
            // Bulk/Interrupt/Iso IN row
            let bgColor = '#17a2b8';
            let typeLabel = 'BULK IN';
            if (actionType === 'interrupt_in') {
                bgColor = '#20c997';
                typeLabel = 'INT IN';
            } else if (actionType === 'iso_in') {
                bgColor = '#ffc107';
                typeLabel = 'ISO IN';
            }
            let configInfo = 'EP' + (req.endpoint || 1) + ', addr=' + (req.deviceAddr || 0) + ', len=' + (req.length || 64) + ', timeout=' + (req.timeout || 1000) + 'ms';
            if (req.continuous) configInfo += ', continuous';
            
            html += '<tr style="background:' + bgColor + '22;">';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">' + (i + 1) + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;color:' + bgColor + ';font-weight:bold;">' + typeLabel + '</td>';
            html += '<td colspan="7" style="padding:4px;border:1px solid #555;">' + configInfo + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">';
            html += '<button onclick="moveChainItem(' + i + ',-1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">^</button>';
            html += '<button onclick="moveChainItem(' + i + ',1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">v</button>';
            html += '<button onclick="removeFromChain(' + i + ')" style="padding:2px 6px;margin:1px;font-size:10px;background:#dc3545;">x</button>';
            html += '</td>';
            html += '</tr>';
        } else if (actionType === 'bulk_out' || actionType === 'interrupt_out' || actionType === 'iso_out') {
            // Bulk/Interrupt/Iso OUT row
            let bgColor = '#17a2b8';
            let typeLabel = 'BULK OUT';
            if (actionType === 'interrupt_out') {
                bgColor = '#20c997';
                typeLabel = 'INT OUT';
            } else if (actionType === 'iso_out') {
                bgColor = '#ffc107';
                typeLabel = 'ISO OUT';
            }
            let configInfo = 'EP' + (req.endpoint || 1) + ', addr=' + (req.deviceAddr || 0) + ', data=' + (req.dataBytes || '(none)');
            
            html += '<tr style="background:' + bgColor + '22;">';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">' + (i + 1) + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;color:' + bgColor + ';font-weight:bold;">' + typeLabel + '</td>';
            html += '<td colspan="7" style="padding:4px;border:1px solid #555;">' + configInfo + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">';
            html += '<button onclick="moveChainItem(' + i + ',-1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">^</button>';
            html += '<button onclick="moveChainItem(' + i + ',1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">v</button>';
            html += '<button onclick="removeFromChain(' + i + ')" style="padding:2px 6px;margin:1px;font-size:10px;background:#dc3545;">x</button>';
            html += '</td>';
            html += '</tr>';
        } else if (actionType === 'condition') {
            // Condition row - purple area color (#6f42c1)
            let bgColor = '#6f42c1';
            let conditionLabel = 'IF ' + (req.operator || '==').toUpperCase();
            const action = req.action || 'continue';
            
            // Format Value A
            let valueAStr = '';
            if (req.valueASource === 'rxbytes' || req.valueASource === 'responsehex') {
                const reqNo = req.valueAReqNo;
                const reqNoStr = reqNo === -1 ? 'last' : (reqNo < 0 ? 'last' + reqNo : '#' + reqNo);
                const length = req.valueALength;
                valueAStr = 'Req' + reqNoStr + '[' + (length === -1 ? 'all' : length + 'B') + ']';
            } else {
                valueAStr = req.valueAValue || 'manual';
            }
            
            // Format Value B
            let valueBStr = '';
            if (req.valueBSource === 'rxbytes' || req.valueBSource === 'responsehex') {
                const reqNo = req.valueBReqNo;
                const reqNoStr = reqNo === -1 ? 'last' : (reqNo < 0 ? 'last' + reqNo : '#' + reqNo);
                const length = req.valueBLength;
                valueBStr = 'Req' + reqNoStr + '[' + (length === -1 ? 'all' : length + 'B') + ']';
            } else {
                valueBStr = req.valueBValue || 'manual';
            }
            
            let configInfo = valueAStr + ' ' + (req.operator || '==') + ' ' + valueBStr + ' -> ' + action;
            
            html += '<tr style="background:' + bgColor + '22;">';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">' + (i + 1) + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;color:' + bgColor + ';font-weight:bold;">' + conditionLabel + '</td>';
            html += '<td colspan="7" style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editConditionConfig(' + i + ',this)">' + configInfo + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">';
            html += '<button onclick="moveChainItem(' + i + ',-1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">^</button>';
            html += '<button onclick="moveChainItem(' + i + ',1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">v</button>';
            html += '<button onclick="removeFromChain(' + i + ')" style="padding:2px 6px;margin:1px;font-size:10px;background:#dc3545;">x</button>';
            html += '</td>';
            html += '</tr>';
        } else {
            // USB request row - red area color (#dc3545)
            let bgColor = '#dc3545';
            const dataPreview = req.dataBytes ? (req.dataBytes.substring(0, 12) + (req.dataBytes.length > 12 ? '...' : '')) : '-';
            // Build flags string
            let flags = [];
            if (req.noRetry) flags.push('<span style="color:#ff9800;font-size:9px;" title="No Retry - expects timeout">NR</span>');
            if (req.deviceAddr && req.deviceAddr !== 0) flags.push('<span style="color:#17a2b8;font-size:9px;" title="Device Address ' + req.deviceAddr + '">A' + req.deviceAddr + '</span>');
            if (req.dataStageEp && req.dataStageEp !== 0) flags.push('<span style="color:#e83e8c;font-size:9px;" title="DATA IN redirected to EP' + req.dataStageEp + '">EP' + req.dataStageEp + '</span>');
            if (req.setupOnly) flags.push('<span style="color:#6f42c1;font-size:9px;" title="Setup Only - skip DATA/STATUS">SO</span>');
            const flagsStr = flags.length > 0 ? ' ' + flags.join(' ') : '';
            html += '<tr style="background:' + bgColor + '22;">';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">' + (i + 1) + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;color:' + bgColor + ';font-weight:bold;">CTRL' + flagsStr + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editChainCell(' + i + ',\'bmRequestType\',this)">' + req.bmRequestType + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editChainCell(' + i + ',\'bRequest\',this)">' + req.bRequest + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editChainCell(' + i + ',\'wValue\',this)">' + req.wValue + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editChainCell(' + i + ',\'wIndex\',this)">' + req.wIndex + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editChainCell(' + i + ',\'wLength\',this)">' + req.wLength + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;cursor:pointer;" onclick="editChainCell(' + i + ',\'packetSize\',this)">' + req.packetSize + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;font-size:10px;cursor:pointer;" onclick="editChainCell(' + i + ',\'dataBytes\',this)">' + dataPreview + '</td>';
            html += '<td style="padding:4px;border:1px solid #555;text-align:center;">';
            html += '<button onclick="moveChainItem(' + i + ',-1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">^</button>';
            html += '<button onclick="moveChainItem(' + i + ',1)" style="padding:2px 6px;margin:1px;font-size:10px;background:#555;">v</button>';
            html += '<button onclick="removeFromChain(' + i + ')" style="padding:2px 6px;margin:1px;font-size:10px;background:#dc3545;">x</button>';
            html += '</td>';
            html += '</tr>';
        }
    });
    tbody.innerHTML = html;
}

function editWaitforConfig(index, cell) {
    if (chainRunning) return;
    
    const req = chainRequests[index];
    if (req.waitType === 'button') {
        const newLabel = prompt('Enter button wait label:', req.label || 'Press Continue to proceed');
        if (newLabel !== null) {
            req.label = newLabel;
            renderChain();
        }
    } else if (req.waitType === 'webhook') {
        const newId = prompt('Enter trigger ID:', req.triggerId || 'trigger1');
        if (newId !== null) {
            req.triggerId = newId;
            renderChain();
        }
    } else if (req.waitType === 'gpio') {
        const newPin = prompt('Enter GPIO pin number (0-48):', req.gpioPin || '0');
        if (newPin === null) return;
        const pinNum = parseInt(newPin);
        if (isNaN(pinNum) || pinNum < 0 || pinNum > 48) {
            alert('Invalid GPIO pin (0-48)');
            return;
        }
        
        const newLevel = prompt('Trigger when HIGH or LOW?', req.gpioLevel ? 'HIGH' : 'LOW').toUpperCase();
        if (newLevel !== 'HIGH' && newLevel !== 'LOW') {
            alert('Invalid level, use HIGH or LOW');
            return;
        }
        
        req.gpioPin = pinNum;
        req.gpioLevel = newLevel === 'HIGH' ? 1 : 0;
        renderChain();
    } else if (req.waitType === 'delay') {
        const newDuration = prompt('Enter delay duration (milliseconds):', req.duration || 1000);
        if (newDuration !== null) {
            const duration = parseInt(newDuration);
            if (isNaN(duration) || duration < 1) {
                alert('Invalid duration (must be >= 1 ms)');
                return;
            }
            req.duration = duration;
            renderChain();
        }
    } else if (req.waitType === 'usb_reset') {
        const newNote = prompt('Enter note (optional):', req.note || 'USB Reset');
        if (newNote !== null) {
            req.note = newNote;
            renderChain();
        }
    }
}

function editActionConfig(index, cell) {
    if (chainRunning) return;
    
    const req = chainRequests[index];
    if (req.actionType === 'http') {
        const newUrl = prompt('Enter URL:', req.url || 'http://example.com/api');
        if (newUrl !== null) {
            req.url = newUrl;
            renderChain();
        }
    } else if (req.actionType === 'gpio_out') {
        const newPin = prompt('Enter GPIO pin number (0-48):', req.gpioPin || '0');
        if (newPin === null) return;
        const pinNum = parseInt(newPin);
        if (isNaN(pinNum) || pinNum < 0 || pinNum > 48) {
            alert('Invalid GPIO pin (0-48)');
            return;
        }
        
        const newLevel = prompt('Output HIGH or LOW?', req.gpioLevel ? 'HIGH' : 'LOW').toUpperCase();
        if (newLevel !== 'HIGH' && newLevel !== 'LOW') {
            alert('Invalid level, use HIGH or LOW');
            return;
        }
        
        req.gpioPin = pinNum;
        req.gpioLevel = newLevel === 'HIGH' ? 1 : 0;
        renderChain();
    } else if (req.actionType === 'comment') {
        const newText = prompt('Enter comment text:', req.text || '');
        if (newText !== null) {
            req.text = newText;
            renderChain();
        }
    } else if (req.actionType === 'copy' || req.actionType === 'copy_to') {
        const newSource = prompt('Copy from source (rxbytes, responsehex):', req.copyFromSource || 'responsehex');
        if (newSource !== null) {
            req.copyFromSource = newSource;
            req.actionType = 'copy';
            const newFromReqNo = prompt('From ReqNo (-1=last):', req.copyFromReqNo !== undefined ? req.copyFromReqNo : '-1');
            if (newFromReqNo !== null) {
                req.copyFromReqNo = parseInt(newFromReqNo) || -1;
                const newFromOffset = prompt('From Offset (byte position, 0=start):', req.copyFromOffset !== undefined ? req.copyFromOffset : '0');
                if (newFromOffset !== null) {
                    req.copyFromOffset = parseInt(newFromOffset) || 0;
                    const newFromLength = prompt('From Length (-1=all from offset):', req.copyFromLength !== undefined ? req.copyFromLength : '-1');
                if (newFromLength !== null) {
                    req.copyFromLength = parseInt(newFromLength) || -1;
                    const newToField = prompt('Paste to field:', req.copyToField || 'wValue');
                    if (newToField !== null) {
                        req.copyToField = newToField;
                        const newToReqNo = prompt('To ReqNo (-1=next):', req.copyToReqNo !== undefined ? req.copyToReqNo : '-1');
                        if (newToReqNo !== null) {
                            req.copyToReqNo = parseInt(newToReqNo) || -1;
                            renderChain();
                            }
                        }
                    }
                }
            }
        }
    } else if (req.actionType === 'goto') {
        const newReqNo = prompt('Go to ReqNo (0-based index):', req.gotoReqNo || '0');
        if (newReqNo !== null) {
            req.gotoReqNo = parseInt(newReqNo) || 0;
            renderChain();
        }
    }
}

function editConditionConfig(index, cell) {
    if (chainRunning) return;
    
    const req = chainRequests[index];
    
    // Edit operator
    const newOp = prompt('Operator (==, !=, <, <=, >, >=, contains):', req.operator || '==');
    if (newOp === null) return;
    req.operator = newOp;
    
    // Edit Value A
    const newASource = prompt('Value A source (rxbytes, responsehex, or manual):', req.valueASource || 'rxbytes');
    if (newASource === null) return;
    req.valueASource = newASource;
    
    if (newASource === 'rxbytes' || newASource === 'responsehex') {
        const newAReqNo = prompt('Value A Request Number (-1=last, -2=last-1, etc.):', req.valueAReqNo !== undefined ? req.valueAReqNo : '-1');
        if (newAReqNo === null) return;
        req.valueAReqNo = parseInt(newAReqNo) || -1;
        
        const newALength = prompt('Value A Length (bytes, -1=all):', req.valueALength !== undefined ? req.valueALength : '-1');
        if (newALength === null) return;
        req.valueALength = parseInt(newALength);
        if (isNaN(req.valueALength)) req.valueALength = -1;
        req.valueAValue = '';
    } else {
        const newAValue = prompt('Value A hex value:', req.valueAValue || '');
        if (newAValue === null) return;
        req.valueAValue = newAValue;
    }
    
    // Edit Value B
    const newBSource = prompt('Value B source (rxbytes, responsehex, or manual):', req.valueBSource || 'manual');
    if (newBSource === null) return;
    req.valueBSource = newBSource;
    
    if (newBSource === 'rxbytes' || newBSource === 'responsehex') {
        const newBReqNo = prompt('Value B Request Number (-1=last, -2=last-1, etc.):', req.valueBReqNo !== undefined ? req.valueBReqNo : '-1');
        if (newBReqNo === null) return;
        req.valueBReqNo = parseInt(newBReqNo) || -1;
        
        const newBLength = prompt('Value B Length (bytes, -1=all):', req.valueBLength !== undefined ? req.valueBLength : '-1');
        if (newBLength === null) return;
        req.valueBLength = parseInt(newBLength);
        if (isNaN(req.valueBLength)) req.valueBLength = -1;
        req.valueBValue = '';
    } else {
        const newBValue = prompt('Value B hex value:', req.valueBValue || '');
        if (newBValue === null) return;
        req.valueBValue = newBValue;
    }
    
    // Edit action
    const newAction = prompt('Then action (continue, skip_next, break):', req.action || 'continue');
    if (newAction === null) return;
    req.action = newAction;
    
    renderChain();
}

function editChainCell(index, field, cell) {
    // Don't allow editing while chain is running
    if (chainRunning) return;
    
    // Get current value
    const currentValue = chainRequests[index][field];
    const displayValue = (field === 'dataBytes' && currentValue) ? currentValue : (currentValue || '');
    
    // Create input field
    const input = document.createElement('input');
    input.type = 'text';
    input.value = displayValue;
    input.style.cssText = 'width:100%;padding:2px;background:#1a1a1a;color:#fff;border:1px solid #ff4444;font-family:monospace;font-size:11px;box-sizing:border-box;';
    
    // Save on blur, Enter, or input change
    const saveValue = () => {
        let newValue = input.value;
        
        // Convert to number for numeric fields
        if (field === 'wLength' || field === 'packetSize' || field === 'deviceAddr' || field === 'dataStageEp') {
            newValue = parseInt(newValue) || 0;
        }
        // Convert to boolean for setupOnly
        if (field === 'setupOnly') {
            newValue = (newValue === 'true' || newValue === '1' || newValue === true);
        }
        
        chainRequests[index][field] = newValue;
        renderChain();
    };
    
    // Save immediately on any input change
    input.oninput = () => {
        let newValue = input.value;
        if (field === 'wLength' || field === 'packetSize' || field === 'deviceAddr' || field === 'dataStageEp') {
            newValue = parseInt(newValue) || 0;
        }
        if (field === 'setupOnly') {
            newValue = (newValue === 'true' || newValue === '1' || newValue === true);
        }
        chainRequests[index][field] = newValue;
    };
    
    input.onblur = saveValue;
    input.onkeydown = (e) => {
        if (e.key === 'Enter') {
            e.preventDefault();
            saveValue();
        } else if (e.key === 'Escape') {
            renderChain(); // Cancel edit
        }
    };
    
    // Replace cell content with input
    cell.innerHTML = '';
    cell.appendChild(input);
    input.focus();
    input.select();
}


function continueChain() {
    if (chainWaitingForContinue && chainContinueResolver) {
        chainContinueResolver();
        chainContinueResolver = null;
    }
}

function stopChain() {
    chainRunning = false;
    chainWaitingForContinue = false;
    if (chainContinueResolver) {
        chainContinueResolver();
        chainContinueResolver = null;
    }
    // Stop native chain
    if (chainWs && chainWs.readyState === WebSocket.OPEN) {
        chainWs.send('stop');
        chainWs.close();
    }
    
    
    document.getElementById('chainProgress').innerText = 'Stopped at step ' + (chainCurrentIndex + 1);
    document.getElementById('chainProgress').style.color = '#dc3545';
    document.getElementById('chainContinueBtn').style.display = 'none';
    document.getElementById('chainWaitInfo').style.display = 'none';
    document.getElementById('chainExecBtn').style.display = 'inline-block';
    document.getElementById('chainStopBtn').style.display = 'none';
}


// Build CSV from current chain
function buildChainCSV() {
    let csv = '# USBane Chain Export\n';
    // Apply configuration tab auto-recovery setting to chain execution
    csv += 'action,config,reset_on_retry,' + (getResetOnRetry() ? '1' : '0') + '\n';
    
    chainRequests.forEach(req => {
        const actionType = req.type || 'control';
        
        if (actionType === 'waitfor') {
            if (req.waitType === 'gpio') {
                csv += 'waitfor,gpio,' + (req.gpioPin || 0) + ',' + (req.gpioLevel || 1) + '\n';
            } else if (req.waitType === 'delay') {
                csv += 'waitfor,delay,' + (req.duration || 1000) + '\n';
            } else if (req.waitType === 'usb_reset') {
                csv += 'waitfor,usb_reset\n';
            } else if (req.waitType === 'vbus_cycle') {
                csv += 'waitfor,vbus_cycle\n';
            } else if (req.waitType === 'button') {
                csv += 'waitfor,button,"' + (req.label || 'Press Continue') + '",' + (req.timeout || 0) + '\n';
            } else if (req.waitType === 'webhook') {
                csv += 'waitfor,webhook,"' + (req.triggerId || '') + '",' + (req.timeout || 300) + '\n';
            } else {
                // Default to button wait
                csv += 'waitfor,button,"' + (req.label || 'Press Continue') + '"\n';
            }
        } else if (actionType === 'action') {
            if (req.actionType === 'copy' || req.actionType === 'copy_to') {
                csv += 'action,copy,' + (req.copyFromSource || 'responsehex') + ',' + 
                       (req.copyFromReqNo ?? -1) + ',' + (req.copyFromOffset || 0) + ',' + 
                       (req.copyFromLength || 2) + ',' + (req.copyToField || 'wValue') + ',' + 
                       (req.copyToReqNo ?? -1) + '\n';
            } else if (req.actionType === 'goto') {
                csv += 'action,goto,' + (req.gotoReqNo || 0) + '\n';
            } else if (req.actionType === 'comment') {
                csv += 'action,comment,"' + (req.text || '') + '"\n';
            } else if (req.actionType === 'gpio_out') {
                csv += 'action,gpio_out,' + (req.gpioPin || 0) + ',' + (req.gpioLevel || 0) + '\n';
            } else if (req.actionType === 'http') {
                csv += 'action,http,"' + (req.url || 'http://example.com') + '",' + (req.method || 'get') + '\n';
            } else if (req.actionType === 'add32') {
                var fieldPart = (req.add32Field && req.add32Field !== 'wValue') ? ',' + req.add32Field : '';
                csv += 'action,add32,' + (req.add32Increment || '0x40') + ',' + (req.add32Entries || [0,0,0,0]).join(',') + fieldPart + '\n';
            }
        } else if (actionType === 'condition') {
            csv += 'condition,' + (req.operator || '==') + ',' + 
                   (req.aSource || 'responsehex') + ',' + (req.aReqNo ?? -1) + ',' + (req.aLength || 1) + ',"' + (req.aValue || '') + '",' +
                   (req.bSource || 'manual') + ',' + (req.bReqNo ?? -1) + ',' + (req.bLength || 1) + ',"' + (req.bValue || '') + '",' +
                   (req.condAction || 'skip') + '\n';
        } else if (actionType === 'bulk_in' || actionType === 'interrupt_in' || actionType === 'iso_in') {
            csv += actionType + ',' + (req.endpoint || 1) + ',' + (req.length || 64) + ',' + 
                   (req.deviceAddr || 0) + ',' + (req.timeout || 1000) + ',' +
                   (req.continuous ? 1 : 0) + ',' + (req.maxAttempts || 1) + '\n';
        } else if (actionType === 'bulk_out' || actionType === 'interrupt_out' || actionType === 'iso_out') {
            csv += actionType + ',' + (req.endpoint || 1) + ',"' + (req.dataBytes || '') + '",' +
                   (req.deviceAddr || 0) + ',' + (req.timeout || 1000) + '\n';
        } else {
            // Control transfer
            let flags = [];
            if (req.noRetry) flags.push('noretry');
            if (req.setupOnly) flags.push('setuponly');
            if (req.dataStageEp) flags.push('ep' + req.dataStageEp);
            
            csv += 'control,' + (req.bmRequestType || '0x00') + ',' + (req.bRequest || '0x00') + ',' +
                   (req.wValue || '0x0000') + ',' + (req.wIndex || '0x0000') + ',' +
                   (req.wLength || 0) + ',' + (req.packetSize || 8) + ',' +
                   (req.dataMode || 'separate') + ',"' + (req.dataBytes || '') + '",' +
                   (req.deviceAddr || 0) + ',' + flags.join('_') + '\n';
        }
    });
    
    return csv;
}

// Native execution via WebSocket
function executeChainNative() {
    if (chainRequests.length === 0) {
        alert('No requests in chain');
        return;
    }
    if (chainRunning) return;
    
    chainRunning = true;
    chainCurrentIndex = 0;
    
    document.getElementById('chainExecBtn').style.display = 'none';
    document.getElementById('chainStopBtn').style.display = 'inline-block';
    document.getElementById('chainContinueBtn').style.display = 'none';
    document.getElementById('chainWaitInfo').style.display = 'none';
    
    const progress = document.getElementById('chainProgress');
    progress.innerText = 'Connecting...';
    progress.style.color = '#17a2b8';
    
    // Clear results table
    document.getElementById('results_table').innerHTML = '';
    
    // Build CSV
    const csv = buildChainCSV();
    console.log('Chain CSV:', csv);
    
    // Connect WebSocket
    const wsUrl = 'ws://' + window.location.host + '/ws/chain';
    chainWs = new WebSocket(wsUrl);
    
    chainWs.onopen = () => {
        progress.innerText = 'Executing...';
        chainWs.send(csv);
    };
    
    chainWs.onmessage = (event) => {
        try {
            const msg = JSON.parse(event.data);
            
            if (msg.status === 'done') {
                progress.innerText = 'Chain completed';
                progress.style.color = '#28a745';
                finishNativeExecution();
                return;
            }
            
            if (msg.status === 'error') {
                progress.innerText = 'Chain error';
                progress.style.color = '#dc3545';
                finishNativeExecution();
                return;
            }
            
            // Handle waiting for button/webhook
            if (msg.waiting === 'button') {
                progress.innerText = 'Waiting for button...';
                progress.style.color = '#ffc107';
                showButtonWaitModal(msg.label || 'Press Continue');
                return;
            }
            
            if (msg.waiting === 'webhook') {
                progress.innerText = 'Waiting for webhook: ' + (msg.trigger_id || '');
                progress.style.color = '#ffc107';
                return;
            }
            
            // Result for entry at index msg.i
            chainCurrentIndex = msg.i;
            progress.innerText = 'Step ' + (msg.i + 1) + '/' + chainRequests.length;
            
            // Map backend type -> action type (keeps UI aligned when CSV includes hidden config actions)
            const chainTypeMap = {
                0: 'control',
                1: 'bulk_in',
                2: 'bulk_out',
                3: 'interrupt_in',
                4: 'interrupt_out',
                5: 'iso_in',
                6: 'iso_out',
                7: 'waitfor',       // WAIT_DELAY
                8: 'waitfor',       // WAIT_GPIO
                9: 'waitfor',       // WAIT_USB_RESET
                10: 'waitfor',      // WAIT_VBUS_CYCLE
                11: 'waitfor',      // WAIT_BUTTON
                12: 'waitfor',      // WAIT_WEBHOOK
                13: 'action',       // ACTION_COPY
                14: 'action',       // ACTION_GOTO
                15: 'action',       // ACTION_COMMENT
                16: 'action',       // ACTION_GPIO_OUT
                17: 'action',       // ACTION_HTTP
                18: 'config',       // ACTION_CONFIG
                19: 'action',       // ACTION_ADD32
                20: 'condition'     // CONDITION
            };
            if (msg.t === 17) {
                // config action inserted by UI; don't render a row
                return;
            }
            const req = chainRequests[msg.i] || {};
            const actionType = chainTypeMap[msg.t] || req.type || 'control';
            
            // Determine status text
            let statusText = msg.s === 0 ? 'OK' : (msg.s === 1 ? 'TIMEOUT' : 'ERROR');
            
            // Format row based on type
            if (actionType === 'control') {
                const bmRT = msg.bmRT !== undefined ? '0x' + msg.bmRT.toString(16).padStart(2, '0') : (req.bmRequestType || '0x00');
                const bReq = msg.bReq !== undefined ? '0x' + msg.bReq.toString(16).padStart(2, '0') : (req.bRequest || '0x00');
                const wVal = msg.wVal !== undefined ? '0x' + msg.wVal.toString(16).padStart(4, '0') : (req.wValue || '0x0000');
                const wIdx = msg.wIdx !== undefined ? '0x' + msg.wIdx.toString(16).padStart(4, '0') : (req.wIndex || '0x0000');
                const wLen = msg.wLen !== undefined ? msg.wLen : (req.wLength || 0);
                const pkt = msg.pkt !== undefined ? msg.pkt : (req.packetSize || 8);
                addTableRow(
                    bmRT,
                    bReq,
                    wVal,
                    wIdx,
                    wLen,
                    pkt,
                    msg.b || 0,
                    msg.d || '',
                    actionType.toUpperCase()
                );
            } else if (actionType.includes('_in') || actionType.includes('_out')) {
                addTableRow(
                    actionType.toUpperCase(),
                    'EP' + (req.endpoint || 1),
                    req.deviceAddr || 0,
                    '',
                    req.length || 0,
                    '',
                    msg.b || 0,
                    msg.d || '',
                    statusText
                );
            } else {
                // Wait/action/condition
                addTableRow(
                    actionType.toUpperCase(),
                    req.waitType || req.actionType || '',
                    '', '', '', '',
                    '',
                    msg.d || '',
                    statusText
                );
            }
            
        } catch (e) {
            console.error('WS parse error:', e);
        }
    };
    
    chainWs.onerror = (err) => {
        console.error('WS error:', err);
        progress.innerText = 'Connection error';
        progress.style.color = '#dc3545';
        finishNativeExecution();
    };
    
    chainWs.onclose = () => {
        if (chainRunning) {
            progress.innerText = 'Connection closed';
            progress.style.color = '#888';
        }
        finishNativeExecution();
    };
}

// Show modal for button wait during chain execution
function showButtonWaitModal(label) {
    // Create modal overlay
    let modal = document.getElementById('buttonWaitModal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'buttonWaitModal';
        modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.7);display:flex;align-items:center;justify-content:center;z-index:10000;';
        modal.innerHTML = `
            <div style="background:#1e1e1e;border:2px solid #ffc107;border-radius:12px;padding:30px 40px;text-align:center;max-width:400px;">
                <div style="font-size:48px;margin-bottom:15px;">⏸️</div>
                <h3 style="color:#ffc107;margin:0 0 15px 0;">Chain Paused</h3>
                <p id="buttonWaitLabel" style="color:#ccc;margin:0 0 25px 0;font-size:14px;"></p>
                <button id="buttonWaitContinue" style="background:#ffc107;color:#000;border:none;padding:12px 30px;font-size:14px;font-weight:bold;border-radius:6px;cursor:pointer;">Continue</button>
            </div>
        `;
        document.body.appendChild(modal);
        
        document.getElementById('buttonWaitContinue').onclick = () => {
            modal.style.display = 'none';
            // Send continue command to backend
            if (chainWs && chainWs.readyState === WebSocket.OPEN) {
                chainWs.send(JSON.stringify({ cmd: 'continue' }));
            }
        };
    }
    
    document.getElementById('buttonWaitLabel').innerText = label;
    modal.style.display = 'flex';
}

function finishNativeExecution() {
    chainRunning = false;
    
    // Hide button wait modal if visible
    const modal = document.getElementById('buttonWaitModal');
    if (modal) modal.style.display = 'none';
    chainWs = null;
    document.getElementById('chainExecBtn').style.display = 'inline-block';
    document.getElementById('chainStopBtn').style.display = 'none';
}

function exportChainCSV() {
    if (chainRequests.length === 0) {
        alert('No requests to export');
        return;
    }

    let csv = 'type,p1,p2,p3,p4,p5,p6,p7,p8,p9,p10\n';
    csv += 'action,config,reset_on_retry,' + (getResetOnRetry() ? '1' : '0') + '\n';
    chainRequests.forEach(req => {
        const actionType = req.type || 'control';
        if (actionType === 'waitfor') {
            if (req.waitType === 'gpio') {
                csv += 'waitfor,gpio,' + req.gpioPin + ',' + req.gpioLevel + '\n';
            } else if (req.waitType === 'webhook') {
                csv += 'waitfor,webhook,"' + (req.triggerId || '') + '"\n';
            } else if (req.waitType === 'delay') {
                csv += 'waitfor,delay,' + (req.duration || 1000) + '\n';
            } else if (req.waitType === 'usb_reset') {
                csv += 'waitfor,usb_reset,"' + (req.note || 'USB Reset') + '"\n';
            } else if (req.waitType === 'vbus_cycle') {
                csv += 'waitfor,vbus_cycle\n';
            } else {
                csv += 'waitfor,button,"' + (req.label || 'Press Continue') + '"\n';
            }
        } else if (actionType === 'action') {
            if (req.actionType === 'http') {
                csv += 'action,http,"' + (req.url || 'http://example.com/api') + '"\n';
            } else if (req.actionType === 'gpio_out') {
                csv += 'action,gpio_out,' + req.gpioPin + ',' + req.gpioLevel + '\n';
            } else if (req.actionType === 'comment') {
                csv += 'action,comment,"' + (req.text || 'Comment') + '"\n';
            } else if (req.actionType === 'copy' || req.actionType === 'copy_to') {
                csv += 'action,copy,' + req.copyFromSource + ',' + (req.copyFromReqNo || -1) + ',' + (req.copyFromOffset || 0) + ',' + (req.copyFromLength || -1) + ',' + (req.copyToField || 'wValue') + ',' + (req.copyToReqNo || -1) + '\n';
            } else if (req.actionType === 'goto') {
                csv += 'action,goto,' + (req.gotoReqNo || 0) + '\n';
            } else if (req.actionType === 'add32') {
                var fieldPart2 = (req.add32Field && req.add32Field !== 'wValue') ? ',' + req.add32Field : '';
                csv += 'action,add32,' + (req.add32Increment || '0x40') + ',' + (req.add32Entries || [0,0,0,0]).join(',') + fieldPart2 + '\n';
            }
        } else if (actionType === 'condition') {
            const op = req.operator || '==';
            const action = req.action || 'continue';
            const aSource = req.valueASource || 'manual';
            const aReqNo = req.valueAReqNo !== undefined ? req.valueAReqNo : -1;
            const aLength = req.valueALength !== undefined ? req.valueALength : -1;
            const aValue = req.valueAValue || '';
            const bSource = req.valueBSource || 'manual';
            const bReqNo = req.valueBReqNo !== undefined ? req.valueBReqNo : -1;
            const bLength = req.valueBLength !== undefined ? req.valueBLength : -1;
            const bValue = req.valueBValue || '';
            csv += 'condition,' + op + ',' + aSource + ',' + aReqNo + ',' + aLength + ',"' + aValue + '",' + bSource + ',' + bReqNo + ',' + bLength + ',"' + bValue + '",' + action + '\n';
        } else if (actionType === 'bulk_in' || actionType === 'interrupt_in' || actionType === 'iso_in') {
            // Format: bulk_in/interrupt_in/iso_in,endpoint,length,deviceAddr,timeout,continuous,max_attempts
            csv += actionType + ',' + (req.endpoint || 1) + ',' + (req.length || 64) + ',' + 
                   (req.deviceAddr || 0) + ',' + (req.timeout || 1000) + ',' +
                   (req.continuous ? 1 : 0) + ',' + (req.maxAttempts || 10) + '\n';
        } else if (actionType === 'bulk_out' || actionType === 'interrupt_out' || actionType === 'iso_out') {
            // Format: bulk_out/interrupt_out/iso_out,endpoint,dataBytes,deviceAddr,timeout
            csv += actionType + ',' + (req.endpoint || 1) + ',"' + (req.dataBytes || '') + '",' +
                   (req.deviceAddr || 0) + ',' + (req.timeout || 1000) + '\n';
        } else {
            // Append flags if set
            let flags = [];
            if (req.noRetry) flags.push('noretry');
            if (req.setupOnly) flags.push('setuponly');
            if (req.dataStageEp) flags.push('ep' + req.dataStageEp);
            let line = [
                'control',
                req.bmRequestType,
                req.bRequest,
                req.wValue,
                req.wIndex,
                req.wLength,
                req.packetSize,
                req.dataMode || 'separate',
                '"' + (req.dataBytes || '') + '"',
                req.deviceAddr || 0,
                flags.join('_')
            ].join(',');
            csv += line + '\n';
        }
    });
    
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'chain_' + new Date().toISOString().slice(0, 19).replace(/:/g, '-') + '.csv';
    a.click();
    URL.revokeObjectURL(url);
}

function importChainCSV() {
    document.getElementById('chainFileInput').click();
}

function handleChainFileImport(event) {
    const file = event.target.files[0];
    if (!file) return;
    
    const reader = new FileReader();
    reader.onload = function(e) {
        const content = e.target.result;
        const lines = content.split('\n');
        
        // Clear existing chain before import
        chainRequests = [];
        
        // Skip header
        let imported = 0;
        for (let i = 1; i < lines.length; i++) {
            const line = lines[i].trim();
            if (!line) continue;
            
            // Skip comment lines
            if (line.startsWith('#')) continue;
            
            // Parse CSV line (handle quoted fields)
            const parts = parseCSVLine(line);
            if (parts.length < 1) continue;
            
            // Format: type,config (type determines remaining fields)
            const actionType = parts[0].toLowerCase();
            
            if (actionType === 'waitfor' && parts.length >= 2) {
                const waitType = parts[1] || 'button';
                if (waitType === 'gpio' && parts.length >= 4) {
                    // waitfor,gpio,pin,level
                    chainRequests.push({
                        type: 'waitfor',
                        waitType: 'gpio',
                        gpioPin: parseInt(parts[2]) || 0,
                        gpioLevel: parseInt(parts[3]) || 0,
                        timeout: 60
                    });
                } else if (waitType === 'webhook') {
                    chainRequests.push({
                        type: 'waitfor',
                        waitType: 'webhook',
                        triggerId: parts[2] || '',
                        timeout: 300
                    });
                } else if (waitType === 'delay' && parts.length >= 3) {
                    chainRequests.push({
                        type: 'waitfor',
                        waitType: 'delay',
                        duration: parseInt(parts[2]) || 1000
                    });
                } else if (waitType === 'usb_reset') {
                    chainRequests.push({
                        type: 'waitfor',
                        waitType: 'usb_reset',
                        note: parts[2] || 'USB Reset'
                    });
                } else if (waitType === 'vbus_cycle') {
                    chainRequests.push({
                        type: 'waitfor',
                        waitType: 'vbus_cycle',
                        note: 'VBUS Power Cycle'
                    });
                } else {
                    chainRequests.push({
                        type: 'waitfor',
                        waitType: 'button',
                        label: parts[2] || 'Press Continue'
                    });
                }
                imported++;
            } else if (actionType === 'action' && parts.length >= 2) {
                const subType = parts[1];
                if (subType === 'config' && parts.length >= 4) {
                    if (parts[2] === 'reset_on_retry') {
                        document.getElementById('cfg_resetOnRetry').checked = parts[3] === '1' || parts[3] === 'true';
                        toggleResetAfterRetries();
                    }
                    imported++;
                    continue;
                }
                if (subType === 'http') {
                    chainRequests.push({
                        type: 'action',
                        actionType: 'http',
                        url: parts[2] || 'http://example.com/api'
                    });
                    imported++;
                } else if (subType === 'gpio_out' && parts.length >= 4) {
                    chainRequests.push({
                        type: 'action',
                        actionType: 'gpio_out',
                        gpioPin: parseInt(parts[2]) || 0,
                        gpioLevel: parseInt(parts[3]) || 0
                    });
                    imported++;
                } else if (subType === 'comment') {
                    chainRequests.push({
                        type: 'action',
                        actionType: 'comment',
                        text: parts[2] || 'Comment'
                    });
                    imported++;
                } else if (subType === 'copy' && parts.length >= 7) {
                    // Format: action,copy,source,fromReqNo,fromOffset,fromLength,toField,toReqNo
                    chainRequests.push({
                        type: 'action',
                        actionType: 'copy',
                        copyFromSource: parts[2] || 'responsehex',
                        copyFromReqNo: parseInt(parts[3]) || -1,
                        copyFromOffset: parseInt(parts[4]) || 0,
                        copyFromLength: parseInt(parts[5]) || -1,
                        copyToField: parts[6] || 'wValue',
                        copyToReqNo: parseInt(parts[7]) || -1
                    });
                    imported++;
                } else if (subType === 'goto' && parts.length >= 3) {
                    chainRequests.push({
                        type: 'action',
                        actionType: 'goto',
                        gotoReqNo: parseInt(parts[2]) || 0
                    });
                    imported++;
                } else if (subType === 'add32' && parts.length >= 7) {
                    // Format: action,add32,increment,e0,e1,e2,e3[,field]
                    var add32Field = (parts.length >= 8 && parts[7]) ? parts[7].trim() : 'wValue';
                    chainRequests.push({
                        type: 'action',
                        actionType: 'add32',
                        add32Increment: parts[2] || '0x40',
                        add32Entries: [parts[3], parts[4], parts[5], parts[6]],
                        add32Field: add32Field
                    });
                    imported++;
                }
            } else if (actionType === 'condition' && parts.length >= 11) {
                // Format: condition,operator,aSource,aReqNo,aLength,aValue,bSource,bReqNo,bLength,bValue,action
                const aLength = parseInt(parts[4]);
                const bLength = parseInt(parts[8]);
                chainRequests.push({
                    type: 'condition',
                    operator: parts[1] || '==',
                    valueASource: parts[2] || 'manual',
                    valueAReqNo: parseInt(parts[3]) || -1,
                    valueALength: isNaN(aLength) ? -1 : aLength,
                    valueAValue: parts[5] || '',
                    valueBSource: parts[6] || 'manual',
                    valueBReqNo: parseInt(parts[7]) || -1,
                    valueBLength: isNaN(bLength) ? -1 : bLength,
                    valueBValue: parts[9] || '',
                    action: parts[10] || 'continue'
                });
                imported++;
            } else if (actionType === 'control' && parts.length >= 7) {
                // Format: control,bmRequestType,bRequest,wValue,wIndex,wLength,packetSize,dataMode,dataBytes,deviceAddr,flags
                // parts[9] = deviceAddr (0=default)
                // parts[10] = flags: noretry, setuponly, epXX (underscore-separated)
                const deviceAddr = parseInt(parts[9]) || 0;
                const flagsStr = (parts[10] || '').toLowerCase().trim();
                const hasNoRetry = flagsStr.includes('noretry');
                const hasSetupOnly = flagsStr.includes('setuponly');
                // Parse dataStageEp from flags (format: ep10, ep5, etc.)
                const epMatch = flagsStr.match(/ep(\d+)/);
                const dataStageEp = epMatch ? parseInt(epMatch[1]) : 0;
                chainRequests.push({
                    type: 'control',
                    bmRequestType: parts[1],
                    bRequest: parts[2],
                    wValue: parts[3],
                    wIndex: parts[4],
                    wLength: parseInt(parts[5]),
                    packetSize: parseInt(parts[6]),
                    dataMode: parts[7] || 'separate',
                    dataBytes: parts[8] || '',
                    dataStageEp: dataStageEp,
                    deviceAddr: deviceAddr,
                    noRetry: hasNoRetry,
                    setupOnly: hasSetupOnly
                });
                imported++;
            } else if ((actionType === 'bulk_in' || actionType === 'interrupt_in' || actionType === 'iso_in') && parts.length >= 3) {
                // Format: bulk_in/interrupt_in/iso_in,endpoint,length,deviceAddr,timeout,continuous,max_attempts
                chainRequests.push({
                    type: actionType,
                    endpoint: parseInt(parts[1]) || 1,
                    length: parseInt(parts[2]) || 64,
                    deviceAddr: parseInt(parts[3]) || 0,
                    timeout: parseInt(parts[4]) || 1000,
                    continuous: parseInt(parts[5]) || 0,
                    maxAttempts: parseInt(parts[6]) || 10,
                    channel: 1  // Default channel
                });
                imported++;
            } else if ((actionType === 'bulk_out' || actionType === 'interrupt_out' || actionType === 'iso_out') && parts.length >= 3) {
                // Format: bulk_out/interrupt_out/iso_out,endpoint,dataBytes,deviceAddr,timeout
                chainRequests.push({
                    type: actionType,
                    endpoint: parseInt(parts[1]) || 1,
                    dataBytes: parts[2] || '',
                    deviceAddr: parseInt(parts[3]) || 0,
                    timeout: parseInt(parts[4]) || 1000,
                    channel: 1  // Default channel
                });
                imported++;
            }
        }
        
        renderChain();
        alert('Imported ' + imported + ' actions');
    };
    reader.readAsText(file);
    
    // Reset file input
    event.target.value = '';
}

function parseCSVLine(line) {
    const result = [];
    let current = '';
    let inQuotes = false;
    
    for (let i = 0; i < line.length; i++) {
        const char = line[i];
        
        if (char === '"') {
            inQuotes = !inQuotes;
        } else if (char === ',' && !inQuotes) {
            result.push(current.trim());
            current = '';
        } else {
            current += char;
        }
    }
    result.push(current.trim());
    
    return result;
}

// ============================================
// End Request Chain Tab
// ============================================

function switchTab(idx) {
    document.querySelectorAll('.tab').forEach((t,i) => t.className = i===idx ? 'tab active' : 'tab');
    document.querySelectorAll('.tab-content').forEach((c,i) => c.className = i===idx ? 'tab-content active' : 'tab-content');
}

function toggleAccordion(header) {
    const content = header.nextElementSibling;
    const isActive = header.classList.contains('active');
    
    // Close all accordions
    document.querySelectorAll('.accordion-header').forEach(h => {
        h.classList.remove('active');
        h.nextElementSibling.classList.remove('active');
    });
    
    // Open clicked one if it was closed
    if (!isActive) {
        header.classList.add('active');
        content.classList.add('active');
    }
}

function clearTable() {
    if(!confirm('Clear all results?')) return;
    document.getElementById('results_table').innerHTML = '';
    req_count = 0;
    allResultsData = []; // Clear the full data store too
}

// Full data store for exports (keeps ALL results, not just visible DOM rows)
let allResultsData = [];

let sortDir = {};

function sortTable(colIdx) {
    let table = document.getElementById('results_table');
    let rows = Array.from(table.getElementsByTagName('tr'));
    if(rows.length === 0) return;
    sortDir[colIdx] = !sortDir[colIdx];
    let asc = sortDir[colIdx];
    rows.sort((a,b) => {
        let aVal = parseInt(a.cells[colIdx].getAttribute('data-sort') || '0');
        let bVal = parseInt(b.cells[colIdx].getAttribute('data-sort') || '0');
        return asc ? (aVal - bVal) : (bVal - aVal);
    });
    table.innerHTML = '';
    rows.forEach(row => table.appendChild(row));
    document.querySelectorAll('[id^="sort_"]').forEach(el => el.innerText = '');
    document.getElementById('sort_' + colIdx).innerText = asc ? '^' : 'v';
}

function addTableRow(bmReqType, bReq, wVal, wIdx, wLen, pktSize, rxBytes, respHex, ascii) {
    req_count++;
    let isFailed = (respHex === 'FAILED' || respHex === 'ERROR');
    let fullHex = (respHex || '').toUpperCase().replace(/\s/g,'');
    let fullAscii = ascii || '';
    
    // Store ALL data for exports (even if DOM row gets pruned)
    allResultsData.push({
        num: req_count,
        bmReqType, bReq, wVal, wIdx, wLen, pktSize, rxBytes,
        respHex: fullHex,
        ascii: fullAscii,
        isFailed
    });
    let row = '<tr data-rxbytes="' + rxBytes + '" data-resphex="' + fullHex + '" data-ascii="' + fullAscii.replace(/"/g, '&quot;') + '" data-failed="' + (isFailed ? '1' : '0') + '">';
    row += '<td data-sort="' + req_count + '" style="padding:4px;border:1px solid #555;">' + req_count + '</td>';
    row += '<td style="padding:4px;border:1px solid #555;">' + bmReqType + '</td>';
    row += '<td style="padding:4px;border:1px solid #555;">' + bReq + '</td>';
    row += '<td style="padding:4px;border:1px solid #555;">' + wVal + '</td>';
    row += '<td style="padding:4px;border:1px solid #555;">' + wIdx + '</td>';
    row += '<td style="padding:4px;border:1px solid #555;">' + wLen + '</td>';
    row += '<td data-sort="' + pktSize + '" style="padding:4px;border:1px solid #555;">' + pktSize + '</td>';
    row += '<td data-sort="' + rxBytes + '" style="padding:4px;border:1px solid #555;">' + rxBytes + '</td>';
    
    // Truncate hex but make clickable to show full
    let respData = fullHex.substring(0, 32) + (fullHex.length > 32 ? '...' : '');
    let hexStyle = fullHex.length > 32 ? 'cursor:pointer;color:#4da6ff;text-decoration:underline;' : '';
    row += '<td style="padding:4px;border:1px solid #555;max-width:150px;overflow:hidden;' + hexStyle + '" onclick="showFullResponse(this.parentNode)">' + respData + '</td>';
    
    // Truncate ascii but make clickable
    let asciiData = fullAscii.substring(0, 16) + (fullAscii.length > 16 ? '...' : '');
    let asciiStyle = fullAscii.length > 16 ? 'cursor:pointer;color:#4da6ff;text-decoration:underline;' : '';
    row += '<td style="padding:4px;border:1px solid #555;max-width:100px;overflow:hidden;' + asciiStyle + '" onclick="showFullResponse(this.parentNode)">' + asciiData + '</td></tr>';
    
    let table = document.getElementById('results_table');
    table.insertAdjacentHTML('afterbegin', row);
    onNewRow();
}

// Show full response data in a modal
function showFullResponse(row) {
    let hex = row.dataset.resphex || '';
    let ascii = row.dataset.ascii || '';
    
    if (!hex && !ascii) return;
    
    // Create or get modal
    let modal = document.getElementById('responseModal');
    if (!modal) {
        modal = document.createElement('div');
        modal.id = 'responseModal';
        modal.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.8);display:flex;align-items:center;justify-content:center;z-index:1000;';
        modal.innerHTML = `
            <div style="background:#1a1a1a;border:1px solid #555;border-radius:8px;padding:20px;max-width:90%;max-height:80%;overflow:auto;">
                <h3 style="margin-top:0;color:#fff;">Response Data</h3>
                <div style="margin-bottom:10px;">
                    <label style="color:#aaa;">Hex (${hex.length/2} bytes):</label>
                    <textarea id="modalHex" readonly style="width:100%;height:150px;background:#0a0a0a;color:#4da6ff;border:1px solid #444;font-family:monospace;font-size:12px;padding:8px;margin-top:4px;"></textarea>
                </div>
                <div style="margin-bottom:15px;">
                    <label style="color:#aaa;">ASCII:</label>
                    <textarea id="modalAscii" readonly style="width:100%;height:80px;background:#0a0a0a;color:#8f8;border:1px solid #444;font-family:monospace;font-size:12px;padding:8px;margin-top:4px;"></textarea>
                </div>
                <div style="text-align:right;">
                    <button onclick="copyResponseToClipboard('hex')" style="padding:8px 16px;background:#444;color:#fff;border:none;cursor:pointer;margin-right:8px;">Copy Hex</button>
                    <button onclick="copyResponseToClipboard('ascii')" style="padding:8px 16px;background:#444;color:#fff;border:none;cursor:pointer;margin-right:8px;">Copy ASCII</button>
                    <button onclick="document.getElementById('responseModal').style.display='none'" style="padding:8px 16px;background:#666;color:#fff;border:none;cursor:pointer;">Close</button>
                </div>
            </div>
        `;
        document.body.appendChild(modal);
        modal.addEventListener('click', (e) => {
            if (e.target === modal) modal.style.display = 'none';
        });
    }
    
    // Format hex with spaces every 2 chars for readability
    let formattedHex = hex.match(/.{1,2}/g)?.join(' ') || '';
    
    document.getElementById('modalHex').value = formattedHex;
    document.getElementById('modalAscii').value = ascii;
    modal.querySelector('label').textContent = 'Hex (' + (hex.length/2) + ' bytes):';
    modal.style.display = 'flex';
}

function copyResponseToClipboard(type) {
    let textarea = document.getElementById(type === 'hex' ? 'modalHex' : 'modalAscii');
    textarea.select();
    document.execCommand('copy');
    
    // Brief visual feedback
    let btn = event.target;
    let origText = btn.textContent;
    btn.textContent = 'Copied!';
    btn.style.background = '#28a745';
    setTimeout(() => {
        btn.textContent = origText;
        btn.style.background = '#444';
    }, 1000);
}

function selectFilterMode(mode) {
    document.getElementById('filter_mode').value = mode;
    if (mode === 'include') {
        document.getElementById('filter_mode_include').style.background = '#ff4444';
        document.getElementById('filter_mode_include').style.color = '#fff';
        document.getElementById('filter_mode_include').style.fontWeight = 'bold';
        document.getElementById('filter_mode_exclude').style.background = '#333';
        document.getElementById('filter_mode_exclude').style.color = '#888';
        document.getElementById('filter_mode_exclude').style.fontWeight = 'normal';
    } else {
        document.getElementById('filter_mode_exclude').style.background = '#ff4444';
        document.getElementById('filter_mode_exclude').style.color = '#fff';
        document.getElementById('filter_mode_exclude').style.fontWeight = 'bold';
        document.getElementById('filter_mode_include').style.background = '#333';
        document.getElementById('filter_mode_include').style.color = '#888';
        document.getElementById('filter_mode_include').style.fontWeight = 'normal';
    }
    applyFilters();
}

function applyFilters() {
    let hideFailed = document.getElementById('filter_hideFailed').checked;
    let minBytes = parseInt(document.getElementById('filter_minBytes').value) || 0;
    let containsStr = (document.getElementById('filter_contains').value || '').toUpperCase().replace(/\s/g,'');
    let filterMode = document.getElementById('filter_mode').value;
    
    // Use children instead of querySelectorAll for better performance
    let table = document.getElementById('results_table');
    let rows = table.children;
    let shown = 0, hidden = 0;
    
    // Process in batches to avoid blocking UI
    for (let i = 0; i < rows.length; i++) {
        let row = rows[i];
        let rxBytes = parseInt(row.dataset.rxbytes) || 0;
        let respHex = row.dataset.resphex || '';
        let isFailed = row.dataset.failed === '1';
        
        let hide = false;
        
        // Hide failed
        if (hideFailed && isFailed) hide = true;
        
        // Min bytes filter (also hides 0-byte responses when minBytes > 0)
        if (rxBytes < minBytes && !isFailed) hide = true;
        
        // Contains/Exclude filter
        if (containsStr) {
            if (filterMode === 'include') {
                // Include mode: hide rows that don't contain the string
                if (!respHex.includes(containsStr)) hide = true;
            } else {
                // Exclude mode: hide rows that contain the string
                if (respHex.includes(containsStr)) hide = true;
            }
        }
        
        row.style.display = hide ? 'none' : '';
        if (hide) hidden++; else shown++;
    }
    
    document.getElementById('filter_stats').innerText = shown + '/' + (shown + hidden) + ' rows';
}

function resetFilters() {
    document.getElementById('filter_hideFailed').checked = false;
    document.getElementById('filter_minBytes').value = '0';
    document.getElementById('filter_contains').value = '';
    selectFilterMode('include');
    applyFilters();
}

// Called when new rows are added - only apply if Live mode is on
function onNewRow() {
    if (document.getElementById('filter_autoUpdate').checked) {
        applyFilters();
    }
}

function exportResults(format) {
    let rows = document.querySelectorAll('#results_table tr');
    let data = [];
    
    rows.forEach(row => {
        if (row.style.display === 'none') return; // Skip hidden rows
        let cells = row.querySelectorAll('td');
        if (cells.length < 9) return;
        // Use data attributes for full hex/ascii (innerText is truncated for display)
        let fullHex = row.dataset.resphex || '';
        let fullAscii = row.dataset.ascii || '';
        data.push({
            num: parseInt(cells[0].innerText) || 0,
            bmReqType: cells[1].innerText,
            bReq: cells[2].innerText,
            wVal: cells[3].innerText,
            wIdx: cells[4].innerText,
            wLen: cells[5].innerText,
            pktSize: cells[6].innerText,
            rxBytes: cells[7].innerText,
            respHex: fullHex,
            ascii: fullAscii
        });
    });
    
    if (data.length === 0) {
        alert('No visible rows to export');
        return;
    }
    
    // Sort by row number ascending (oldest/first request at top)
    data.sort((a, b) => a.num - b.num);
    
    let output = '';
    let filename = 'usb_dump_' + new Date().toISOString().slice(0,19).replace(/:/g,'-');
    
    if (format === 'csv') {
        output = '#,bmReqType,bReq,wVal,wIdx,wLen,PktSize,RxBytes,Response,ASCII\n';
        data.forEach(d => {
            output += [d.num, d.bmReqType, d.bReq, d.wVal, d.wIdx, d.wLen, d.pktSize, d.rxBytes, '"'+d.respHex+'"', '"'+d.ascii+'"'].join(',') + '\n';
        });
        filename += '.csv';
    } else if (format === 'bin') {
        // Binary format - combine all hex responses in chronological order (oldest first)
        let hexStr = '';
        data.forEach(d => {
            let hex = d.respHex.replace(/\s/g,'');
            if (hex && hex !== 'FAILED' && hex !== 'ERROR') {
                hexStr += hex;
            }
        });
        // Convert hex to binary blob
        let bytes = new Uint8Array(hexStr.length / 2);
        for (let i = 0; i < bytes.length; i++) {
            bytes[i] = parseInt(hexStr.substr(i * 2, 2), 16);
        }
        let blob = new Blob([bytes], {type: 'application/octet-stream'});
        let url = URL.createObjectURL(blob);
        let a = document.createElement('a');
        a.href = url;
        a.download = filename + '.bin';
        a.click();
        URL.revokeObjectURL(url);
        return;
    }
    
    // Download as text
    let blob = new Blob([output], {type: 'text/plain'});
    let url = URL.createObjectURL(blob);
    let a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
}

function updateConnStatus() {
    fetch('/api/status')
    .then(r => r.json())
    .then(d => {
        let el = document.getElementById('connStatus');
        if(d.connected) {
            el.className = 'conn-status conn-connected';
            el.innerText = 'Connected';
            el.style.background = '';
        } else {
            el.className = 'conn-status conn-disconnected';
            el.innerText = 'No Device';
            el.style.background = '';
        }
    })
    .catch(e => {
        let el = document.getElementById('connStatus');
        el.className = 'conn-status';
        el.style.background = '#ff9800';
        el.innerText = 'Offline';
    });
}

// Device status polling - enabled by default (only reads HW register, no USB traffic)
setInterval(updateConnStatus, 2000);

function updateThroughput() {
    fetch('/api/stats')
    .then(r => r.json())
    .then(d => {
        let el = document.getElementById('throughputStats');
        let rxKB = (d.bytes_rx_per_sec / 1024).toFixed(1);
        let txKB = (d.bytes_tx_per_sec / 1024).toFixed(1);
        let cpu0 = d.cpu_core0_load || 0;
        let cpu1 = d.cpu_core1_load || 0;
        
        // RAM usage
        let heapUsed = d.heap_total - d.heap_free;
        let heapUsedKB = (heapUsed / 1024).toFixed(0);
        let heapTotalKB = (d.heap_total / 1024).toFixed(0);
        let heapPercent = ((heapUsed / d.heap_total) * 100).toFixed(0);
        
        el.innerHTML = d.requests_per_sec + ' req/s | down: ' + rxKB + 'KB/s | up: ' + txKB + 'KB/s | CPU0: ' + cpu0 + '% | CPU1: ' + cpu1 + '% | RAM: ' + heapUsedKB + '/' + heapTotalKB + 'KB (' + heapPercent + '%)';
        
        // Keep neutral gray background always
        el.style.background = '#333';
        el.style.color = '#aaa';
    })
    .catch(e => {
        let el = document.getElementById('throughputStats');
        el.innerHTML = '0 req/s | down: 0.0 KB/s | up: 0.0 KB/s | CPU0: 0% | CPU1: 0% | RAM: 0/0 KB (0%)';
        el.style.background = '#333';
        el.style.color = '#888';
    });
}

// Stats polling - ALWAYS runs (CPU/RAM/throughput don't interfere with USB exploits)
setInterval(updateThroughput, 500);

function saveConfig() {
    if(!confirm('Save configuration and reboot ESP32?')) return;
    
    // Read from localStorage (already set by toggle functions)
    let otgMode = localStorage.getItem('cfg_otgMode') || '0';
    let usbBackend = localStorage.getItem('cfg_usbBackend') || 'dwc2';
    let softDp = document.getElementById('cfg_softHostDpPin').value || '16';
    let softDm = document.getElementById('cfg_softHostDmPin').value || '17';
    
    // Save other settings to localStorage
    localStorage.setItem('cfg_maxRetries', document.getElementById('cfg_maxRetries').value);
    localStorage.setItem('cfg_timeout', document.getElementById('cfg_timeout').value);
    localStorage.setItem('cfg_devAddr', document.getElementById('cfg_devAddr').value);
    localStorage.setItem('cfg_endpoint', document.getElementById('cfg_endpoint').value);
    localStorage.setItem('cfg_maxPacketSize', document.getElementById('cfg_maxPacketSize').value);
    localStorage.setItem('cfg_bfMaxRetries', document.getElementById('cfg_bfMaxRetries').value);
    localStorage.setItem('cfg_retryDelayIncrement', document.getElementById('cfg_retryDelayIncrement').value);
    localStorage.setItem('cfg_resetOnRetry', document.getElementById('cfg_resetOnRetry').checked);
    localStorage.setItem('cfg_resetAfterRetries', document.getElementById('cfg_resetAfterRetries').value);
    localStorage.setItem('cfg_wifi_mode', currentWifiMode);
    localStorage.setItem('cfg_softHostDpPin', softDp);
    localStorage.setItem('cfg_softHostDmPin', softDm);
    
    let s = document.getElementById('cfg_status');
    s.innerText = 'Saving and rebooting...';
    s.style.display = 'block';
    s.style.color = '#ff9800';
    
    // Build WiFi config params
    let wifiParams = '&wifiMode=' + currentWifiMode;
    if (currentWifiMode === 'ap') {
        wifiParams += '&apSsid=' + encodeURIComponent(document.getElementById('cfg_ap_ssid').value);
        wifiParams += '&apPassword=' + encodeURIComponent(document.getElementById('cfg_ap_password').value);
    } else {
        wifiParams += '&staSsid=' + encodeURIComponent(document.getElementById('cfg_sta_ssid').value);
        wifiParams += '&staPassword=' + encodeURIComponent(document.getElementById('cfg_sta_password').value);
    }
    
    // Build USB backend params
    let usbParams = '&usbBackend=' + usbBackend + '&softDp=' + softDp + '&softDm=' + softDm;
    
    fetch('/api/save_config?otgMode=' + otgMode + usbParams + wifiParams, { method: 'POST' })
    .then(r => r.json())
    .then(d => {
        s.innerText = d.message || d.status;
        if(d.status === 'success') {
            s.style.color = '#28a745';
            setTimeout(() => { s.innerText = 'Waiting for reboot...'; }, 2000);
        } else {
            s.style.color = '#dc3545';
            setTimeout(() => s.style.display = 'none', 3000);
        }
    })
    .catch(e => { s.innerText = 'Error: ' + e; s.style.color = '#dc3545'; });
}

function factoryReset() {
    if(!confirm('Factory Reset will erase ALL settings (WiFi, USB config, etc.) and reboot.\n\nAre you sure?')) return;
    
    let s = document.getElementById('cfg_status');
    s.innerText = 'Erasing NVS and rebooting...';
    s.style.display = 'block';
    s.style.color = '#ff9800';
    
    fetch('/api/factory_reset', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
        s.innerText = d.message || d.status;
        if(d.status === 'success') {
            s.style.color = '#28a745';
            // Clear localStorage as well
            localStorage.clear();
            setTimeout(() => { s.innerText = 'Waiting for reboot... (Reconnect to default WiFi)'; }, 2000);
        } else {
            s.style.color = '#dc3545';
            setTimeout(() => s.style.display = 'none', 3000);
        }
    })
    .catch(e => { s.innerText = 'Error: ' + e; s.style.color = '#dc3545'; });
}

// ============================================
// WiFi Configuration
// ============================================

let currentWifiMode = 'ap';

function selectWifiMode(mode) {
    currentWifiMode = mode;
    
    if (mode === 'ap') {
        document.getElementById('wifi_mode_ap').style.background = '#ff4444';
        document.getElementById('wifi_mode_ap').style.color = '#fff';
        document.getElementById('wifi_mode_ap').style.fontWeight = 'bold';
        document.getElementById('wifi_mode_sta').style.background = '#333';
        document.getElementById('wifi_mode_sta').style.color = '#888';
        document.getElementById('wifi_mode_sta').style.fontWeight = 'normal';
        document.getElementById('wifi_ap_config').style.display = 'block';
        document.getElementById('wifi_sta_config').style.display = 'none';
    } else {
        document.getElementById('wifi_mode_sta').style.background = '#ff4444';
        document.getElementById('wifi_mode_sta').style.color = '#fff';
        document.getElementById('wifi_mode_sta').style.fontWeight = 'bold';
        document.getElementById('wifi_mode_ap').style.background = '#333';
        document.getElementById('wifi_mode_ap').style.color = '#888';
        document.getElementById('wifi_mode_ap').style.fontWeight = 'normal';
        document.getElementById('wifi_ap_config').style.display = 'none';
        document.getElementById('wifi_sta_config').style.display = 'block';
    }
    
    localStorage.setItem('cfg_wifi_mode', mode);
}

function selectUsbBackend(backend) {
    const dwc2Btn = document.getElementById('usb_backend_dwc2');
    const softBtn = document.getElementById('usb_backend_soft');
    const softPinsConfig = document.getElementById('soft_host_pins_config');
    
    if (backend === 'dwc2') {
        dwc2Btn.style.background = '#ff4444';
        dwc2Btn.style.color = '#fff';
        dwc2Btn.style.fontWeight = 'bold';
        softBtn.style.background = '#333';
        softBtn.style.color = '#888';
        softBtn.style.fontWeight = 'normal';
        softPinsConfig.style.display = 'none';
        localStorage.setItem('cfg_usbBackend', 'dwc2');
    } else {
        softBtn.style.background = '#ff4444';
        softBtn.style.color = '#fff';
        softBtn.style.fontWeight = 'bold';
        dwc2Btn.style.background = '#333';
        dwc2Btn.style.color = '#888';
        dwc2Btn.style.fontWeight = 'normal';
        softPinsConfig.style.display = 'block';
        localStorage.setItem('cfg_usbBackend', 'soft');
    }
}

function getUsbBackend() {
    return localStorage.getItem('cfg_usbBackend') || 'dwc2';
}

function getSoftHostPins() {
    return {
        dpPin: parseInt(document.getElementById('cfg_softHostDpPin').value) || 16,
        dmPin: parseInt(document.getElementById('cfg_softHostDmPin').value) || 17
    };
}

function selectOtgMode(mode) {
    if (mode === 'host') {
        document.getElementById('otg_mode_host').style.background = '#ff4444';
        document.getElementById('otg_mode_host').style.color = '#fff';
        document.getElementById('otg_mode_host').style.fontWeight = 'bold';
        document.getElementById('otg_mode_device').style.background = '#333';
        document.getElementById('otg_mode_device').style.color = '#888';
        document.getElementById('otg_mode_device').style.fontWeight = 'normal';
        localStorage.setItem('cfg_otgMode', '0');
    } else {
        document.getElementById('otg_mode_device').style.background = '#ff4444';
        document.getElementById('otg_mode_device').style.color = '#fff';
        document.getElementById('otg_mode_device').style.fontWeight = 'bold';
        document.getElementById('otg_mode_host').style.background = '#333';
        document.getElementById('otg_mode_host').style.color = '#888';
        document.getElementById('otg_mode_host').style.fontWeight = 'normal';
        localStorage.setItem('cfg_otgMode', '1');
    }
}

function loadWifiConfig() {
    // Load from localStorage
    const savedMode = localStorage.getItem('cfg_wifi_mode');
    if (savedMode) {
        selectWifiMode(savedMode);
    }
    
    // Try to get current WiFi config from ESP32
    fetch('/api/wifi_config')
    .then(r => r.json())
    .then(d => {
        if (d.mode) {
            selectWifiMode(d.mode);
        }
        if (d.ap_ssid) {
            document.getElementById('cfg_ap_ssid').value = d.ap_ssid;
        }
        if (d.sta_ssid) {
            document.getElementById('cfg_sta_ssid').value = d.sta_ssid;
        }
    })
    .catch(e => {});
}

function loadConfig() {
    // Load USB Backend toggle
    const usbBackend = localStorage.getItem('cfg_usbBackend');
    if (usbBackend === 'soft') {
        selectUsbBackend('soft');
    } else {
        selectUsbBackend('dwc2');
    }
    
    // Load OTG Mode toggle
    const otgMode = localStorage.getItem('cfg_otgMode');
    if (otgMode === '1') {
        selectOtgMode('device');
    } else {
        selectOtgMode('host');
    }
    
    if(localStorage.getItem('cfg_maxRetries')) document.getElementById('cfg_maxRetries').value = localStorage.getItem('cfg_maxRetries');
    if(localStorage.getItem('cfg_timeout')) document.getElementById('cfg_timeout').value = localStorage.getItem('cfg_timeout');
    if(localStorage.getItem('cfg_devAddr')) document.getElementById('cfg_devAddr').value = localStorage.getItem('cfg_devAddr');
    if(localStorage.getItem('cfg_endpoint')) document.getElementById('cfg_endpoint').value = localStorage.getItem('cfg_endpoint');
    if(localStorage.getItem('cfg_maxPacketSize')) document.getElementById('cfg_maxPacketSize').value = localStorage.getItem('cfg_maxPacketSize');
    if(localStorage.getItem('cfg_bfMaxRetries')) document.getElementById('cfg_bfMaxRetries').value = localStorage.getItem('cfg_bfMaxRetries');
    if(localStorage.getItem('cfg_retryDelayIncrement')) document.getElementById('cfg_retryDelayIncrement').value = localStorage.getItem('cfg_retryDelayIncrement');
    if(localStorage.getItem('cfg_resetOnRetry') !== null) document.getElementById('cfg_resetOnRetry').checked = localStorage.getItem('cfg_resetOnRetry') === 'true';
    if(localStorage.getItem('cfg_resetAfterRetries')) document.getElementById('cfg_resetAfterRetries').value = localStorage.getItem('cfg_resetAfterRetries');
    toggleResetAfterRetries(); // Apply initial state
    
    // Load soft-host GPIO pins
    if(localStorage.getItem('cfg_softHostDpPin')) document.getElementById('cfg_softHostDpPin').value = localStorage.getItem('cfg_softHostDpPin');
    if(localStorage.getItem('cfg_softHostDmPin')) document.getElementById('cfg_softHostDmPin').value = localStorage.getItem('cfg_softHostDmPin');
    
    // Load WiFi config
    loadWifiConfig();
}

loadConfig();

// Compare semantic versions (e.g., "1.2.3" vs "1.3.0")
function compareVersions(v1, v2) {
    const parts1 = v1.replace(/^v/, '').split('.').map(Number);
    const parts2 = v2.replace(/^v/, '').split('.').map(Number);
    
    for (let i = 0; i < Math.max(parts1.length, parts2.length); i++) {
        const p1 = parts1[i] || 0;
        const p2 = parts2[i] || 0;
        if (p1 > p2) return 1;
        if (p1 < p2) return -1;
    }
    return 0;
}

// Fetch and display app version with update check
function fetchAppVersion() {
    let currentVersion = '';
    
    // Fetch current version from device
    fetch('/api/version')
    .then(r => r.json())
    .then(d => {
        if (d.version) {
            currentVersion = d.version;
            document.getElementById('appVersion').innerText = 'v' + currentVersion;
            
            // Check for updates on GitHub (with timeout)
            const controller = new AbortController();
            const timeout = setTimeout(() => controller.abort(), 5000); // 5 second timeout
            
            return fetch('https://api.github.com/repos/Fehr-GmbH/USBane/releases/latest', {
                signal: controller.signal,
                headers: { 'Accept': 'application/vnd.github.v3+json' }
            }).finally(() => clearTimeout(timeout));
        }
    })
    .then(r => {
        if (!r) return null;
        
        // Check if request was successful
        if (!r.ok) {
            // 404 means no releases yet, or network issue
            throw new Error(r.status === 404 ? 'No releases found' : 'GitHub API error: ' + r.status);
        }
        
        return r.json();
    })
    .then(release => {
        if (release && release.tag_name && currentVersion) {
            const latestVersion = release.tag_name.replace(/^v/, '');
            const current = currentVersion.replace(/^v/, '');
            
            if (compareVersions(latestVersion, current) > 0) {
                // Update available
                const versionElem = document.getElementById('appVersion');
                versionElem.innerHTML = 'v' + currentVersion + ' <span style="background:#ff9800;padding:2px 6px;border-radius:8px;font-size:9px;margin-left:4px;cursor:pointer;animation:pulse 2s infinite;" title="Update available: v' + latestVersion + '">UPDATE</span>';
                versionElem.style.cursor = 'pointer';
                versionElem.onclick = () => window.open('https://github.com/Fehr-GmbH/USBane/releases', '_blank');
                versionElem.title = 'New version v' + latestVersion + ' available! Click to view releases.';
                
            }
        }
    })
    .catch(e => {
        // Show warning when update check fails
        if (currentVersion) {
            const versionElem = document.getElementById('appVersion');
            
            // Determine the appropriate warning message
            let title = 'Update check failed';
            if (e.name === 'AbortError') {
                title = 'Update check timeout';
            } else if (e.message.includes('No releases found')) {
                title = 'No releases available yet on GitHub';
            } else {
                title = 'Update check failed: ' + e.message;
            }
            
            versionElem.innerHTML = 'v' + currentVersion + ' <span style="color:#ff9800;font-size:10px;margin-left:4px;cursor:help;font-weight:bold;" title="' + title + '">!</span>';
        }
    });
}

fetchAppVersion();

// Initialize stats and connection status immediately
updateThroughput();
updateConnStatus();

// Bidirectional sync: Update dropdowns when input values change
function syncDropdownToInput(inputId, dropdownSelector) {
    const input = document.getElementById(inputId);
    const dropdown = document.querySelector(dropdownSelector);
    
    if (!input || !dropdown) return;
    
    input.addEventListener('input', function() {
        const value = this.value.toLowerCase();
        // Try to find matching option
        for (let option of dropdown.options) {
            if (option.value.toLowerCase() === value) {
                dropdown.value = option.value;
                return;
            }
        }
        // No match found - reset dropdown to "Quick Select..."
        dropdown.value = '';
    });
}

// Sync dropdown to current input value (for initial load)
function initDropdownValue(inputId, dropdownSelector) {
    const input = document.getElementById(inputId);
    const dropdown = document.querySelector(dropdownSelector);
    
    if (!input || !dropdown) return;
    
    const value = input.value.toLowerCase();
    // Try to find matching option
    for (let option of dropdown.options) {
        if (option.value.toLowerCase() === value) {
            dropdown.value = option.value;
            return;
        }
    }
}

// Segmented control for DATA Mode (Single Request)
function selectDataMode(mode) {
    document.getElementById('dataMode').value = mode;
    if (mode === 'separate') {
        document.getElementById('dataMode_separate').style.background = '#ff4444';
        document.getElementById('dataMode_separate').style.color = '#fff';
        document.getElementById('dataMode_separate').style.fontWeight = 'bold';
        document.getElementById('dataMode_append').style.background = '#333';
        document.getElementById('dataMode_append').style.color = '#888';
        document.getElementById('dataMode_append').style.fontWeight = 'normal';
    } else {
        document.getElementById('dataMode_append').style.background = '#ff4444';
        document.getElementById('dataMode_append').style.color = '#fff';
        document.getElementById('dataMode_append').style.fontWeight = 'bold';
        document.getElementById('dataMode_separate').style.background = '#333';
        document.getElementById('dataMode_separate').style.color = '#888';
        document.getElementById('dataMode_separate').style.fontWeight = 'normal';
    }
    updatePacketSizeOnDataMode();
}

// ============================================================
// Bulk/Interrupt Endpoint Transfer Functions
// ============================================================

var currentEpDirection = 'in';
var currentEpType = 'bulk';

function selectEpDirection(dir) {
    currentEpDirection = dir;
    const inBtn = document.getElementById('ep_dir_in');
    const outBtn = document.getElementById('ep_dir_out');
    const lengthLabel = document.getElementById('ep_length_label');
    const lengthInput = document.getElementById('ep_length');
    const dataLabel = document.getElementById('ep_data_label');
    const dataInput = document.getElementById('ep_data_out');
    
    if (dir === 'in') {
        inBtn.style.background = '#17a2b8';
        inBtn.style.color = '#fff';
        inBtn.style.fontWeight = 'bold';
        outBtn.style.background = '#333';
        outBtn.style.color = '#888';
        outBtn.style.fontWeight = 'normal';
        lengthLabel.textContent = 'Length';
        lengthInput.disabled = false;
        lengthInput.style.background = '#333';
        dataInput.placeholder = 'Not used for IN transfers';
        dataInput.disabled = true;
        dataInput.style.background = '#1a1a1a';
    } else {
        outBtn.style.background = '#17a2b8';
        outBtn.style.color = '#fff';
        outBtn.style.fontWeight = 'bold';
        inBtn.style.background = '#333';
        inBtn.style.color = '#888';
        inBtn.style.fontWeight = 'normal';
        lengthLabel.textContent = 'Length (auto)';
        lengthInput.disabled = true;
        lengthInput.style.background = '#1a1a1a';
        dataInput.placeholder = 'DEADBEEF... (hex bytes to send)';
        dataInput.disabled = false;
        dataInput.style.background = '#333';
    }
}

function selectEpType(type) {
    console.log('selectEpType called with:', type);
    currentEpType = type;
    const bulkBtn = document.getElementById('ep_type_bulk');
    const intBtn = document.getElementById('ep_type_interrupt');

    if (!bulkBtn || !intBtn) {
        console.error('Bulk/Interrupt buttons not found');
        return;
    }

    if (type === 'bulk') {
        bulkBtn.style.background = '#17a2b8';
        bulkBtn.style.color = '#fff';
        bulkBtn.style.fontWeight = 'bold';
        intBtn.style.background = '#333';
        intBtn.style.color = '#888';
        intBtn.style.fontWeight = 'normal';
    } else if (type === 'interrupt') {
        intBtn.style.background = '#17a2b8';
        intBtn.style.color = '#fff';
        intBtn.style.fontWeight = 'bold';
        bulkBtn.style.background = '#333';
        bulkBtn.style.color = '#888';
        bulkBtn.style.fontWeight = 'normal';
    }
    console.log('EpType set to:', currentEpType);
}

async function sendEndpointRequest() {
    console.log('sendEndpointRequest called, currentEpDirection:', currentEpDirection, 'currentEpType:', currentEpType);
    const endpoint = parseInt(document.getElementById('ep_endpoint').value) || 1;
    const deviceAddr = parseInt(document.getElementById('ep_device_addr').value) || 0;
    const channel = parseInt(document.getElementById('ep_channel').value) || 1;
    const maxAttemptsElem = document.getElementById('ep_max_attempts');
    const maxAttempts = maxAttemptsElem ? parseInt(maxAttemptsElem.value) || 10 : 10;
    const continuous = (maxAttempts === -1);
    const length = parseInt(document.getElementById('ep_length').value) || 64;
    const timeout = parseInt(document.getElementById('ep_timeout').value) || 1000;
    const dataOut = document.getElementById('ep_data_out').value.replace(/\s/g, '');

    console.log('Parameters:', {endpoint, deviceAddr, channel, continuous, maxAttempts, length, timeout, dataOut});

    const btn = document.getElementById('sendEpBtn');
    const origText = btn.textContent;
    btn.textContent = 'Sending...';
    btn.disabled = true;
    
    try {
        let url, response, data;
        
        if (currentEpDirection === 'in') {
            // Endpoint IN (read from device)
            url = '/api/endpoint_in?ep=' + endpoint +
                  '&addr=' + deviceAddr +
                  '&channel=' + channel +
                  '&len=' + length +
                  '&timeout=' + timeout +
                  '&type=' + currentEpType +
                  '&continuous=' + (continuous ? 1 : 0) +
                  '&max_attempts=' + maxAttempts;
            response = await fetch(url);
            data = await response.json();
            
            // Add to results table
            addTableRow('EP' + endpoint + '_IN', '-', '-', '-', '-', length, 
                        data.bytes_received || 0, data.data || 'FAILED', data.ascii || '');
        } else {
            // Endpoint OUT (write to device)
            url = '/api/endpoint_out?ep=' + endpoint +
                  '&addr=' + deviceAddr +
                  '&channel=' + channel +
                  '&timeout=' + timeout +
                  '&data=' + dataOut +
                  '&type=' + currentEpType;
            response = await fetch(url);
            data = await response.json();
            
            // Add to results table
            const bytesSent = dataOut.length / 2;
            addTableRow('EP' + endpoint + '_OUT', '-', '-', '-', '-', bytesSent,
                        data.bytes_sent || 0, data.status || 'FAILED', dataOut.substring(0, 32) + (dataOut.length > 32 ? '...' : ''));
        }
        
        console.log('Endpoint transfer result:', data);
        
    } catch (err) {
        console.error('Endpoint transfer error:', err);
        addTableRow('EP' + endpoint + '_' + currentEpDirection.toUpperCase(), '-', '-', '-', '-', length,
                    0, 'ERROR: ' + err.message, '');
    } finally {
        btn.textContent = origText;
        btn.disabled = false;
    }
}

function addEndpointToChain() {
    const endpoint = parseInt(document.getElementById('ep_endpoint').value) || 1;
    const deviceAddr = parseInt(document.getElementById('ep_device_addr').value) || 0;
    const channel = parseInt(document.getElementById('ep_channel').value) || 1;
    const maxAttemptsElem = document.getElementById('ep_max_attempts');
    const maxAttempts = maxAttemptsElem ? parseInt(maxAttemptsElem.value) || 10 : 10;
    const continuous = (maxAttempts === -1);
    const length = parseInt(document.getElementById('ep_length').value) || 64;
    const timeout = parseInt(document.getElementById('ep_timeout').value) || 1000;
    const dataOut = document.getElementById('ep_data_out').value.replace(/\s/g, '');
    
    // Determine type based on direction and transfer type
    let reqType;
    if (currentEpType === 'bulk') {
        reqType = currentEpDirection === 'in' ? 'bulk_in' : 'bulk_out';
    } else {
        reqType = currentEpDirection === 'in' ? 'interrupt_in' : 'interrupt_out';
    }
    
    if (currentEpDirection === 'in') {
        chainRequests.push({
            type: reqType,
            endpoint: endpoint,
            deviceAddr: deviceAddr,
            channel: channel,
            length: length,
            timeout: timeout,
            continuous: continuous,
            maxAttempts: maxAttempts
        });
    } else {
        chainRequests.push({
            type: reqType,
            endpoint: endpoint,
            deviceAddr: deviceAddr,
            channel: channel,
            dataBytes: dataOut,
            timeout: timeout
        });
    }
    
    renderChain();
    
    // Switch to chain tab
    switchTab(1);
}

// ============================================================

// Segmented control for DATA Mode (Bruteforce)
function selectBFDataMode(mode) {
    document.getElementById('bf_dataMode').value = mode;
    if (mode === 'separate') {
        document.getElementById('bf_dataMode_separate').style.background = '#ff4444';
        document.getElementById('bf_dataMode_separate').style.color = '#fff';
        document.getElementById('bf_dataMode_separate').style.fontWeight = 'bold';
        document.getElementById('bf_dataMode_append').style.background = '#333';
        document.getElementById('bf_dataMode_append').style.color = '#888';
        document.getElementById('bf_dataMode_append').style.fontWeight = 'normal';
    } else {
        document.getElementById('bf_dataMode_append').style.background = '#ff4444';
        document.getElementById('bf_dataMode_append').style.color = '#fff';
        document.getElementById('bf_dataMode_append').style.fontWeight = 'bold';
        document.getElementById('bf_dataMode_separate').style.background = '#333';
        document.getElementById('bf_dataMode_separate').style.color = '#888';
        document.getElementById('bf_dataMode_separate').style.fontWeight = 'normal';
    }
    updateBFPacketSizeOnDataMode();
}

// Validate hex input - only allow 0-9, A-F, a-f, x (for 0x prefix), and spaces
function validateHexInput(input) {
    const cursorPos = input.selectionStart;
    const oldValue = input.value;
    
    // Remove any non-hex characters (allow 0x prefix and spaces for readability)
    const newValue = oldValue.replace(/[^0-9A-Fa-fxX\s]/g, '');
    
    if (oldValue !== newValue) {
        input.value = newValue;
        // Restore cursor position (accounting for removed characters)
        const diff = oldValue.length - newValue.length;
        input.setSelectionRange(cursorPos - diff, cursorPos - diff);
        
        // Visual feedback: flash red border briefly
        input.style.borderColor = '#dc3545';
        setTimeout(() => {
            input.style.borderColor = '#555';
        }, 300);
    } else {
        input.style.borderColor = '#555';
    }
}

// Auto-adjust packet size when "Append to SETUP" mode is selected
function updatePacketSizeOnDataMode() {
    const dataMode = document.getElementById('dataMode').value;
    const dataBytes = document.getElementById('dataBytes').value.trim();
    const packetSizeInput = document.getElementById('packetSize');
    
    if (dataMode === 'append') {
        if (dataBytes.length > 0) {
            // Count hex bytes (remove spaces and count pairs)
            const cleanHex = dataBytes.replace(/[^0-9a-fA-F]/g, '');
            const byteCount = Math.floor(cleanHex.length / 2);
            if (byteCount > 0) {
                packetSizeInput.value = 8 + byteCount;
            } else {
                packetSizeInput.value = 8;
            }
        } else {
            // Data field is empty, reset to 8
            packetSizeInput.value = 8;
        }
    } else if (dataMode === 'separate') {
        // Reset to standard SETUP size
        packetSizeInput.value = 8;
    }
}

function updateBFPacketSizeOnDataMode() {
    const dataMode = document.getElementById('bf_dataMode').value;
    const dataBytes = document.getElementById('bf_dataBytes_start').value.trim();
    const packetSizeInput = document.getElementById('bf_packetSize_start');
    
    if (dataMode === 'append') {
        if (dataBytes.length > 0) {
            // Count hex bytes (remove spaces and count pairs)
            const cleanHex = dataBytes.replace(/[^0-9a-fA-F]/g, '');
            const byteCount = Math.floor(cleanHex.length / 2);
            if (byteCount > 0) {
                packetSizeInput.value = 8 + byteCount;
            } else {
                packetSizeInput.value = 8;
            }
        } else {
            // Data field is empty, reset to 8
            packetSizeInput.value = 8;
        }
    } else if (dataMode === 'separate') {
        // Reset to standard SETUP size
        packetSizeInput.value = 8;
    }
}

// Initialize bidirectional syncing for all fields
document.addEventListener('DOMContentLoaded', function() {
    // Set initial dropdown values to match input defaults
    initDropdownValue('bmRequestType', 'select[onchange*="bmRequestType"]');
    initDropdownValue('bRequest', 'select[onchange*="bRequest"]');
    initDropdownValue('wValue', 'select[onchange*="wValue"]');
    initDropdownValue('wIndex', 'select[onchange*="wIndex"]');
    initDropdownValue('wLength', 'select[onchange*="wLength"]');
    initDropdownValue('packetSize', 'select[onchange*="packetSize"]');
    
    // Enable live syncing when input changes
    syncDropdownToInput('bmRequestType', 'select[onchange*="bmRequestType"]');
    syncDropdownToInput('bRequest', 'select[onchange*="bRequest"]');
    syncDropdownToInput('wValue', 'select[onchange*="wValue"]');
    syncDropdownToInput('wIndex', 'select[onchange*="wIndex"]');
    syncDropdownToInput('wLength', 'select[onchange*="wLength"]');
    syncDropdownToInput('packetSize', 'select[onchange*="packetSize"]');
});

function sendReset() {
    if(!confirm('Send USB Reset to device?')) return;
    fetch('/api/reset', { method: 'POST' })
    .then(r => r.json())
    .then(d => alert('USB Reset: ' + d.status))
    .catch(e => alert('Error: ' + e));
}

function sendReboot() {
    if(!confirm('Reboot ESP32? This will reset the entire USB stack.')) return;
    fetch('/api/reboot', { method: 'POST' })
    .then(r => r.json())
    .then(d => {
        alert('Rebooting ESP32...');
        setTimeout(() => location.reload(), 3000);
    })
    .catch(e => alert('Reboot initiated (connection lost is expected)'));
}

async function sendRequest() {
    let btn = document.getElementById('sendBtn');
    btn.style.background = '#dc3545';
    btn.innerText = 'Sending...';
    btn.disabled = true;
    
    let bmRequestType = document.getElementById('bmRequestType').value;
    let bRequest = document.getElementById('bRequest').value;
    let wValue = document.getElementById('wValue').value;
    let wIndex = document.getElementById('wIndex').value;
    let wLength = parseInt(document.getElementById('wLength').value);
    let packetSize = parseInt(document.getElementById('packetSize').value);
    let timeout = parseInt(document.getElementById('cfg_timeout').value) || 1000;
    let dataBytes = document.getElementById('dataBytes').value;
    let deviceAddr = parseInt(document.getElementById('ctrl_deviceAddr').value) || 0;
    let dataStageEp = parseInt(document.getElementById('ctrl_dataStageEp').value) || 0;
    let setupOnly = document.getElementById('ctrl_setupOnly').checked;
    
    try {
        const result = await sendNativeCommand({
            cmd: 'single',
            bmRequestType: bmRequestType,
            bRequest: bRequest,
            wValue: wValue,
            wIndex: wIndex,
            wLength: wLength,
            packetSize: packetSize,
            timeout: timeout,
            data: dataBytes,
            deviceAddr: deviceAddr,
            dataStageEp: dataStageEp,
            setupOnly: setupOnly
        });
        
        // Convert hex to ASCII
        let ascii = '';
        if (result.d) {
            for (let i = 0; i < result.d.length; i += 2) {
                const byte = parseInt(result.d.substr(i, 2), 16);
                ascii += (byte >= 32 && byte < 127) ? String.fromCharCode(byte) : '.';
            }
        }
        
        addTableRow(bmRequestType, bRequest, wValue, wIndex, wLength, packetSize, 
                    result.b || 0, result.d || '', ascii);
        
    } catch (e) {
        console.error('Request failed:', e);
        addTableRow(bmRequestType, bRequest, wValue, wIndex, wLength, packetSize, 
                    0, '', 'ERROR');
    }
    
    btn.style.background = '#28a745';
    btn.innerText = 'Execute Request';
    btn.disabled = false;
}

// ============================================
// Single Request Import/Export/Clear
// ============================================

function exportSingleRequestCSV() {
    let csv = 'field,value\n';
    csv += 'bmRequestType,' + document.getElementById('bmRequestType').value + '\n';
    csv += 'bRequest,' + document.getElementById('bRequest').value + '\n';
    csv += 'wValue,' + document.getElementById('wValue').value + '\n';
    csv += 'wIndex,' + document.getElementById('wIndex').value + '\n';
    csv += 'wLength,' + document.getElementById('wLength').value + '\n';
    csv += 'packetSize,' + document.getElementById('packetSize').value + '\n';
    csv += 'dataMode,' + document.getElementById('dataMode').value + '\n';
    csv += 'dataBytes,"' + document.getElementById('dataBytes').value + '"\n';
    
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'single_request_' + new Date().toISOString().slice(0, 19).replace(/:/g, '-') + '.csv';
    a.click();
    URL.revokeObjectURL(url);
}

function importSingleRequestCSV() {
    document.getElementById('singleReqFileInput').click();
}

function handleSingleRequestFileImport(event) {
    const file = event.target.files[0];
    if (!file) return;
    
    const reader = new FileReader();
    reader.onload = function(e) {
        const content = e.target.result;
        const lines = content.split('\n');
        
        // Skip header
        for (let i = 1; i < lines.length; i++) {
            const line = lines[i].trim();
            if (!line) continue;
            
            const parts = parseCSVLine(line);
            const field = parts[0];
            const value = parts[1] || '';
            
            if (field === 'bmRequestType') document.getElementById('bmRequestType').value = value;
            else if (field === 'bRequest') document.getElementById('bRequest').value = value;
            else if (field === 'wValue') document.getElementById('wValue').value = value;
            else if (field === 'wIndex') document.getElementById('wIndex').value = value;
            else if (field === 'wLength') document.getElementById('wLength').value = value;
            else if (field === 'packetSize') document.getElementById('packetSize').value = value;
            else if (field === 'dataMode') selectDataMode(value || 'separate');
            else if (field === 'dataBytes') document.getElementById('dataBytes').value = value;
        }
        
        alert('Single request configuration imported');
    };
    reader.readAsText(file);
    
    // Reset file input
    event.target.value = '';
}

function clearSingleRequest() {
    if (!confirm('Reset all fields to defaults?')) return;
    
    document.getElementById('bmRequestType').value = '0x80';
    document.getElementById('bRequest').value = '0x06';
    document.getElementById('wValue').value = '0x0100';
    document.getElementById('wIndex').value = '0x0000';
    document.getElementById('wLength').value = '18';
    document.getElementById('packetSize').value = '8';
    document.getElementById('dataBytes').value = '';
    selectDataMode('separate');
}

let bf_running = false;
let bf_count = 0;
let bf_retries = 0;
function getBfMaxRetries() {
    return parseInt(document.getElementById('cfg_bfMaxRetries')?.value) || 3;
}
function getRetryDelayIncrement() {
    return parseInt(document.getElementById('cfg_retryDelayIncrement')?.value) || 500;
}
function getResetOnRetry() {
    return document.getElementById('cfg_resetOnRetry')?.checked !== false;
}
function getResetAfterRetries() {
    return parseInt(document.getElementById('cfg_resetAfterRetries')?.value) || 2;
}

function toggleResetAfterRetries() {
    const enabled = document.getElementById('cfg_resetOnRetry').checked;
    const field = document.getElementById('cfg_resetAfterRetries');
    field.disabled = !enabled;
    field.style.background = enabled ? '' : '#1a1a1a';
    field.style.opacity = enabled ? '1' : '0.6';
}

function toggleBFField(field) {
    let enabled = document.getElementById('bf_' + field).checked;
    // Start field is always enabled (used as fixed value when not bruteforcing)
    // Only end and exclude fields are disabled when checkbox is unchecked
    let endField = document.getElementById('bf_' + field + '_end');
    let excludeField = document.getElementById('bf_' + field + '_exclude');
    
    if (endField) {
        endField.disabled = !enabled;
        endField.style.background = enabled ? '#333' : '#1a1a1a';
    }
    
    // excludeField might not exist for special fields like dataBytes (which has mode toggle instead)
    if (excludeField) {
        excludeField.disabled = !enabled;
        excludeField.style.background = enabled ? '#333' : '#1a1a1a';
    }
}

function parseHexOrDec(val) {
    return val.startsWith('0x') ? parseInt(val, 16) : parseInt(val, 10);
}

function parseExcludeList(excludeStr) {
    if (!excludeStr || excludeStr.trim().length === 0) return [];
    return excludeStr.split(',').map(v => parseHexOrDec(v.trim())).filter(v => !isNaN(v));
}

let bfSocket = null;

async function startBruteforce() {
    if(bf_running) return;
    bf_running = true;
    bf_count = 0;
    bf_retries = 0;
    document.getElementById('bf_start').style.display = 'none';
    document.getElementById('bf_stop').style.display = 'inline-block';
    document.getElementById('bf_progress').style.display = 'block';
    document.getElementById('bf_progress').innerText = 'Starting native bruteforce...';
    
    // Build field configs for native execution
    const fieldNames = ['bmRequestType', 'bRequest', 'wValueHi', 'wValueLo', 'wIndexHi', 'wIndexLo', 'wLength', 'packetSize'];
    const cmd = {
        cmd: 'bruteforce',
        delay: parseInt(document.getElementById('bf_delay').value) || 50
    };
    
    let hasIteration = false;
    fieldNames.forEach(name => {
        const iterate = document.getElementById('bf_' + name).checked;
        const start = parseHexOrDec(document.getElementById('bf_' + name + '_start').value);
        const end = parseHexOrDec(document.getElementById('bf_' + name + '_end').value);
        
        cmd[name] = {
            start: start,
            end: iterate ? end : start,
            iterate: iterate
        };
        
        if (iterate) hasIteration = true;
    });
    
    if (!hasIteration) {
        alert('Select at least one field to bruteforce');
        stopBruteforce();
        return;
    }
    
    try {
        bfSocket = await getUsbSocket();
        
        bfSocket.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                
                if (data.status === 'done') {
                    document.getElementById('bf_progress').innerText = 'Complete. Total requests: ' + (data.total || bf_count);
                    stopBruteforce();
                    return;
                }
                
                if (data.i !== undefined) {
                    bf_count = data.i + 1;
                    document.getElementById('bf_progress').innerText = 'Request ' + bf_count;
                    
                    // Convert hex to ASCII
                    let ascii = '';
                    if (data.d) {
                        for (let i = 0; i < data.d.length; i += 2) {
                            const byte = parseInt(data.d.substr(i, 2), 16);
                            ascii += (byte >= 32 && byte < 127) ? String.fromCharCode(byte) : '.';
                        }
                    }
                    
                    // Format USB params for display
                    const bmRT = data.bmRT !== undefined ? '0x' + data.bmRT.toString(16).padStart(2, '0') : '';
                    const bReq = data.bReq !== undefined ? '0x' + data.bReq.toString(16).padStart(2, '0') : '';
                    const wVal = data.wVal !== undefined ? '0x' + data.wVal.toString(16).padStart(4, '0') : '';
                    const wIdx = data.wIdx !== undefined ? '0x' + data.wIdx.toString(16).padStart(4, '0') : '';
                    const wLen = data.wLen !== undefined ? data.wLen : '';
                    const pkt = data.pkt !== undefined ? data.pkt : '';
                    
                    addTableRow(bmRT, bReq, wVal, wIdx, wLen, pkt, 
                                data.b || 0, data.d || '', 
                                data.s === 0 ? ascii : 'FAILED');
                }
            } catch (e) {
                console.error('BF parse error:', e);
            }
        };
        
        bfSocket.send(JSON.stringify(cmd));
        
    } catch (e) {
        console.error('Bruteforce failed:', e);
        document.getElementById('bf_progress').innerText = 'Error: ' + e.message;
        stopBruteforce();
    }
}

function stopBruteforce() {
    bf_running = false;
    
    if (bfSocket && bfSocket.readyState === WebSocket.OPEN) {
        bfSocket.send('stop');
    }
    
    document.getElementById('bf_start').style.display = 'inline-block';
    document.getElementById('bf_stop').style.display = 'none';
    document.getElementById('bf_progress').innerText = 'Stopped. Total requests: ' + bf_count;
}

// ============================================
// Bruteforce Import/Export/Clear
// ============================================

function exportBruteforceCSV() {
    const fields = ['bmRequestType', 'bRequest', 'wValueHi', 'wValueLo', 'wIndexHi', 'wIndexLo', 'wLength', 'packetSize'];
    let csv = 'field,iterate,start,end,exclude\n';
    
    fields.forEach(field => {
        const iterate = document.getElementById('bf_' + field).checked ? '1' : '0';
        const start = document.getElementById('bf_' + field + '_start').value;
        const end = document.getElementById('bf_' + field + '_end').value;
        const exclude = document.getElementById('bf_' + field + '_exclude').value;
        csv += [field, iterate, start, end, '"' + exclude + '"'].join(',') + '\n';
    });
    
    // Add data mode and data bytes
    csv += 'dataMode,' + document.getElementById('bf_dataMode').value + ',,,\n';
    csv += 'dataBytes,"' + document.getElementById('bf_dataBytes_start').value + '",,,\n';
    csv += 'delay,' + document.getElementById('bf_delay').value + ',,,\n';
    
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'bruteforce_config_' + new Date().toISOString().slice(0, 19).replace(/:/g, '-') + '.csv';
    a.click();
    URL.revokeObjectURL(url);
}

function importBruteforceCSV() {
    document.getElementById('bfFileInput').click();
}

function handleBruteforceFileImport(event) {
    const file = event.target.files[0];
    if (!file) return;
    
    const reader = new FileReader();
    reader.onload = function(e) {
        const content = e.target.result;
        const lines = content.split('\n');
        
        // Skip header
        for (let i = 1; i < lines.length; i++) {
            const line = lines[i].trim();
            if (!line) continue;
            
            const parts = parseCSVLine(line);
            const field = parts[0];
            
            if (field === 'dataMode') {
                selectBFDataMode(parts[1] || 'separate');
            } else if (field === 'dataBytes') {
                document.getElementById('bf_dataBytes_start').value = parts[1] || '';
            } else if (field === 'delay') {
                document.getElementById('bf_delay').value = parts[1] || '50';
            } else if (['bmRequestType', 'bRequest', 'wValueHi', 'wValueLo', 'wIndexHi', 'wIndexLo', 'wLength', 'packetSize'].includes(field)) {
                const iterate = parts[1] === '1';
                document.getElementById('bf_' + field).checked = iterate;
                document.getElementById('bf_' + field + '_start').value = parts[2] || '';
                document.getElementById('bf_' + field + '_end').value = parts[3] || '';
                document.getElementById('bf_' + field + '_end').disabled = !iterate;
                document.getElementById('bf_' + field + '_exclude').value = parts[4] || '';
                document.getElementById('bf_' + field + '_exclude').disabled = !iterate;
            }
        }
        
        alert('Bruteforce configuration imported');
    };
    reader.readAsText(file);
    
    // Reset file input
    event.target.value = '';
}

function clearBruteforce() {
    if (!confirm('Reset all bruteforce fields to defaults?')) return;
    
    const defaults = {
        bmRequestType: { start: '0x80', end: '0xFF' },
        bRequest: { start: '0x06', end: '0xFF' },
        wValueHi: { start: '0x01', end: '0x0F' },
        wValueLo: { start: '0x00', end: '0xFF' },
        wIndex: { start: '0x0000', end: '0xFFFF' },
        wLength: { start: '64', end: '255' },
        packetSize: { start: '8', end: '256' }
    };
    
    Object.keys(defaults).forEach(field => {
        document.getElementById('bf_' + field).checked = false;
        document.getElementById('bf_' + field + '_start').value = defaults[field].start;
        document.getElementById('bf_' + field + '_end').value = defaults[field].end;
        document.getElementById('bf_' + field + '_end').disabled = true;
        document.getElementById('bf_' + field + '_exclude').value = '';
        document.getElementById('bf_' + field + '_exclude').disabled = true;
    });
    
    document.getElementById('bf_dataBytes_start').value = '';
    document.getElementById('bf_dataBytes_end').value = '';
    document.getElementById('bf_dataBytes').checked = false;
    document.getElementById('bf_dataBytes_end').disabled = true;
    document.getElementById('bf_delay').value = '50';
    selectBFDataMode('separate');
}

// Device Info Functions
function exportDeviceInfoCSV() {
    const csv = [
        'Field,Value',
        'USB Version,' + (document.getElementById('info_usb_ver').innerText || '-'),
        'Vendor ID,' + (document.getElementById('info_vid').innerText || '-'),
        'Product ID,' + (document.getElementById('info_pid').innerText || '-'),
        'Device Version,' + (document.getElementById('info_dev_ver').innerText || '-'),
        'Class,' + (document.getElementById('info_class').innerText || '-'),
        'Subclass,' + (document.getElementById('info_subclass').innerText || '-'),
        'Protocol,' + (document.getElementById('info_protocol').innerText || '-'),
        'Max Packet Size,' + (document.getElementById('info_mps').innerText || '-'),
        'Num Configurations,' + (document.getElementById('info_num_cfg').innerText || '-'),
        'Languages,' + (document.getElementById('info_langs').innerText || '-'),
        'Manufacturer,' + (document.getElementById('info_manufacturer').innerText || '-'),
        'Product,' + (document.getElementById('info_product').innerText || '-'),
        'Serial Number,' + (document.getElementById('info_serial').innerText || '-'),
        'Config Length,' + (document.getElementById('info_cfg_len').innerText || '-'),
        'Num Interfaces,' + (document.getElementById('info_num_if').innerText || '-'),
        'Config Value,' + (document.getElementById('info_cfg_val').innerText || '-'),
        'Attributes,' + (document.getElementById('info_cfg_attr').innerText || '-'),
        'Max Power,' + (document.getElementById('info_max_pwr').innerText || '-')
    ].join('\n');
    
    const blob = new Blob([csv], {type: 'text/csv'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'device_info_' + new Date().toISOString().split('T')[0] + '.csv';
    a.click();
    URL.revokeObjectURL(url);
}

function clearDeviceInfo() {
    if (!confirm('Clear all device information?')) return;
    
    ['info_usb_ver', 'info_vid', 'info_pid', 'info_dev_ver', 'info_class', 
     'info_subclass', 'info_protocol', 'info_mps', 'info_num_cfg', 'info_langs',
     'info_manufacturer', 'info_product', 'info_serial', 'info_cfg_len',
     'info_num_if', 'info_cfg_val', 'info_cfg_attr', 'info_max_pwr'].forEach(id => {
        const el = document.getElementById(id);
        if (el) el.innerText = '-';
    });
    
    document.getElementById('devDescSection').style.display = 'none';
    document.getElementById('cfgDescSection').style.display = 'none';
    document.getElementById('strDescSection').style.display = 'none';
    document.getElementById('rawDescSection').style.display = 'none';
    document.getElementById('interfacesSection').innerHTML = '';
    
    const status = document.getElementById('deviceInfoStatus');
    if (status) {
        status.innerText = 'Device information cleared';
        status.style.color = '#888';
    }
}

// Configuration Functions
function exportConfigCSV() {
    const csv = [
        'Setting,Value',
        'USB Speed,' + (localStorage.getItem('cfg_usbSpeed') === '0' ? 'Low-Speed' : 'Full-Speed'),
        'OTG Mode,' + (localStorage.getItem('cfg_otgMode') === '1' ? 'Device' : 'Host'),
        'Max Retries,' + (document.getElementById('cfg_maxRetries').value || '100'),
        'Timeout (ms),' + (document.getElementById('cfg_timeout').value || '1000'),
        'Device Address,' + (document.getElementById('cfg_devAddr').value || '1'),
        'Endpoint,' + (document.getElementById('cfg_endpoint').value || '0'),
        'Max Packet Size,' + (document.getElementById('cfg_maxPacketSize').value || '8'),
        'BF Max Retries,' + (document.getElementById('cfg_bfMaxRetries').value || '5'),
        'Retry Delay Increment,' + (document.getElementById('cfg_retryDelayIncrement').value || '100'),
        'Reset on Retry,' + (document.getElementById('cfg_resetOnRetry').checked),
        'Reset After Retries,' + (document.getElementById('cfg_resetAfterRetries').value || '0'),
        'WiFi Mode,' + currentWifiMode,
        'AP SSID,' + (document.getElementById('cfg_ap_ssid').value || ''),
        'AP Password,' + (document.getElementById('cfg_ap_password').value || ''),
        'Router SSID,' + (document.getElementById('cfg_sta_ssid').value || ''),
        'Router Password,' + (document.getElementById('cfg_sta_password').value || '')
    ].join('\n');
    
    const blob = new Blob([csv], {type: 'text/csv'});
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'usbane_config_' + new Date().toISOString().split('T')[0] + '.csv';
    a.click();
    URL.revokeObjectURL(url);
}

function handleConfigFileImport(event) {
    const file = event.target.files[0];
    if (!file) return;
    
    const reader = new FileReader();
    reader.onload = function(e) {
        const content = e.target.result;
        const lines = content.split('\n');
        
        // Skip header
        for (let i = 1; i < lines.length; i++) {
            const line = lines[i].trim();
            if (!line) continue;
            
            const [key, value] = line.split(',');
            if (!key || value === undefined) continue;
            
            const k = key.trim();
            const v = value.trim();
            
            switch(k) {
                case 'USB Backend':
                    selectUsbBackend(v === 'GPIO Soft-Host' ? 'soft' : 'dwc2');
                    break;
                case 'OTG Mode':
                    selectOtgMode(v === 'Device' ? 'device' : 'host');
                    break;
                case 'Max Retries':
                    document.getElementById('cfg_maxRetries').value = v;
                    break;
                case 'Timeout (ms)':
                    document.getElementById('cfg_timeout').value = v;
                    break;
                case 'Device Address':
                    document.getElementById('cfg_devAddr').value = v;
                    break;
                case 'Endpoint':
                    document.getElementById('cfg_endpoint').value = v;
                    break;
                case 'Max Packet Size':
                    document.getElementById('cfg_maxPacketSize').value = v;
                    break;
                case 'BF Max Retries':
                    document.getElementById('cfg_bfMaxRetries').value = v;
                    break;
                case 'Retry Delay Increment':
                    document.getElementById('cfg_retryDelayIncrement').value = v;
                    break;
                case 'Reset on Retry':
                    document.getElementById('cfg_resetOnRetry').checked = v === 'true';
                    break;
                case 'Reset After Retries':
                    document.getElementById('cfg_resetAfterRetries').value = v;
                    break;
                case 'WiFi Mode':
                    selectWifiMode(v);
                    break;
                case 'AP SSID':
                    document.getElementById('cfg_ap_ssid').value = v;
                    break;
                case 'AP Password':
                    document.getElementById('cfg_ap_password').value = v;
                    break;
                case 'Router SSID':
                    document.getElementById('cfg_sta_ssid').value = v;
                    break;
                case 'Router Password':
                    document.getElementById('cfg_sta_password').value = v;
                    break;
            }
        }
        
        alert('Configuration imported successfully!');
    };
    reader.readAsText(file);
    
    // Reset file input
    event.target.value = '';
}

function clearConfig() {
    if (!confirm('Reset all configuration to defaults? This will NOT save to device.')) return;
    
    // Reset to defaults
    selectUsbBackend('dwc2');
    selectOtgMode('host');
    document.getElementById('cfg_maxRetries').value = '100';
    document.getElementById('cfg_timeout').value = '1000';
    document.getElementById('cfg_devAddr').value = '1';
    document.getElementById('cfg_endpoint').value = '0';
    document.getElementById('cfg_maxPacketSize').value = '8';
    document.getElementById('cfg_bfMaxRetries').value = '5';
    document.getElementById('cfg_retryDelayIncrement').value = '100';
    document.getElementById('cfg_resetOnRetry').checked = false;
    document.getElementById('cfg_resetAfterRetries').value = '0';
    selectWifiMode('ap');
    document.getElementById('cfg_ap_ssid').value = 'USBane';
    document.getElementById('cfg_ap_password').value = '';
    document.getElementById('cfg_sta_ssid').value = '';
    document.getElementById('cfg_sta_password').value = '';
    
    alert('Configuration reset to defaults. Click "Save Configuration" to apply.');
}

